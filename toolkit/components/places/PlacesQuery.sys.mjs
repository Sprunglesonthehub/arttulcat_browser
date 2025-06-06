/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BinarySearch: "resource://gre/modules/BinarySearch.sys.mjs",
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  DeferredTask: "resource://gre/modules/DeferredTask.sys.mjs",
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const BULK_PLACES_EVENTS_THRESHOLD = 50;
const OBSERVER_DEBOUNCE_RATE_MS = 500;
const OBSERVER_DEBOUNCE_TIMEOUT_MS = 5000;

/**
 * An object that contains details of a page visit.
 *
 * @typedef {object} HistoryVisit
 *
 * @property {Date} date
 *   When this page was visited.
 * @property {string} title
 *   The page's title.
 * @property {string} url
 *   The page's URL.
 * @property {string} guid
 *   The page's GUID.
 */

/**
 * Cache key type depends on how visits are currently being grouped.
 *
 * By date: number - The start of day timestamp of the visit.
 * By site: string - The domain name of the visit.
 *
 * @typedef {number | string} CacheKey
 */

/**
 * Sorting by date or site, cache is stored as: (Date/Site) => List of Visits.
 * Sorting by date and site, cache is stored as: (Date) => (Site) => List of Visits.
 * Sorting by last visited, cache is stored as: List of Visits.
 *
 * @typedef {Map<CacheKey, HistoryVisit[]> | Map<CacheKey, Map<CacheKey, HistoryVisit[]>> | HistoryVisit[]} CachedHistory
 */

/**
 * Types returnable from the observer.
 *
 * @typedef {PlacesVisitRemoved | PlacesVisit | PlacesHistoryCleared | PlacesVisitTitle} PlacesEventObserved
 */

/**
 * Queries the places database using an async read only connection. Maintains
 * an internal cache of query results which is live-updated by adding listeners
 * to `PlacesObservers`. When the results are no longer needed, call `close` to
 * remove the listeners.
 */
export class PlacesQuery {
  /** @type {CachedHistory} */
  cachedHistory = null;
  /** @type {object} */
  cachedHistoryOptions = null;
  /** @type {Map<string, Set<HistoryVisit>>} */
  #cachedHistoryPerUrl = null;
  /** @type {function(PlacesEventObserved[]): any} */
  #historyListener = null;
  /** @type {function(CachedHistory): any} */
  #historyListenerCallback = null;
  /** @type {DeferredTask} */
  #historyObserverTask = null;

  /**
   * Indicates whether this query is closed. When closed, caches should not be
   * populated, and observers should not be instantiated. It can be reopened by
   * calling `initializeCache()`.
   *
   * @type {boolean}
   */
  #isClosed = false;

  #searchInProgress = false;

  /**
   * Get a snapshot of history visits at this moment.
   *
   * @param {object} [options]
   *   Options to apply to the database query.
   * @param {number} [options.daysOld]
   *   The maximum number of days to go back in history.
   * @param {number} [options.limit]
   *   The maximum number of visits to return.
   * @param {string} [options.sortBy]
   *   The sorting order of history visits:
   *   - "date": Group visits based on the date they occur.
   *   - "site": Group visits based on host, excluding any "www." prefix.
   *   - "datesite": Group visits based on date, then sub-group based on host.
   *   - "lastvisited": Ungrouped list of visits sorted by recency.
   * @returns {Promise<CachedHistory>}
   *   History visits obtained from the database query.
   */
  async getHistory({ daysOld = 60, limit, sortBy = "date" } = {}) {
    const options = { daysOld, limit, sortBy };
    const cacheInvalid =
      this.cachedHistory == null ||
      !lazy.ObjectUtils.deepEqual(options, this.cachedHistoryOptions);
    if (cacheInvalid) {
      this.initializeCache(options);
      await this.fetchHistory();
    }
    if (!this.#historyListener && !this.#isClosed) {
      this.#initHistoryListener();
    }
    return this.cachedHistory;
  }

  /**
   * Clear existing cache and store options for the new query.
   *
   * @param {object} options
   *   The database query options.
   */
  initializeCache(options = this.cachedHistoryOptions) {
    if (options.sortBy === "lastvisited") {
      this.cachedHistory = [];
    } else {
      this.cachedHistory = new Map();
    }
    this.cachedHistoryOptions = options;
    this.#cachedHistoryPerUrl = new Map();
    this.#isClosed = false;
  }

  /**
   * Run the database query and populate the history cache.
   */
  async fetchHistory() {
    const { daysOld, limit, sortBy } = this.cachedHistoryOptions;
    const db = await lazy.PlacesUtils.promiseDBConnection();
    let groupBy;
    switch (sortBy) {
      case "date":
      case "datesite":
        groupBy = "url, date(visit_date / 1000000, 'unixepoch', 'localtime')";
        break;
      case "site":
      case "lastvisited":
        groupBy = "url";
        break;
    }
    const whereClause =
      daysOld == Infinity
        ? ""
        : `WHERE visit_date >= (strftime('%s','now','localtime','start of day','-${Number(
            daysOld
          )} days','utc') * 1000000)`;
    const sql = `SELECT MAX(visit_date) as visit_date, title, url, guid
      FROM moz_historyvisits v
      JOIN moz_places h
      ON v.place_id = h.id
      AND hidden = 0
      ${whereClause}
      GROUP BY ${groupBy}
      ORDER BY visit_date DESC
      LIMIT ${limit > 0 ? limit : -1}`;
    const rows = await db.executeCached(sql);
    if (this.#isClosed) {
      // Do not cache visits if this instance is closed already.
      return;
    }
    for (const row of rows) {
      const visit = this.formatRowAsVisit(row);
      this.#appendToCache(visit);
    }
  }

  /**
   * Search the database for visits matching a search query. This does not
   * affect internal caches, and observers will not be notified of search
   * results obtained from this query.
   *
   * @param {string} query
   *   The search query.
   * @param {number} [limit]
   *   The maximum number of visits to return.
   * @returns {Promise<HistoryVisit[]>}
   *   The matching visits.
   */
  async searchHistory(query, limit) {
    const { sortBy } = this.cachedHistoryOptions;
    const db = await lazy.PlacesUtils.promiseLargeCacheDBConnection();
    let orderBy;
    switch (sortBy) {
      case "date":
        orderBy = "visit_date DESC";
        break;
      case "site":
        orderBy = "url";
        break;
    }
    const sql = `SELECT MAX(visit_date) as visit_date, title, url, guid
      FROM moz_historyvisits v
      JOIN moz_places h
      ON v.place_id = h.id
      WHERE AUTOCOMPLETE_MATCH(:query, url, title, NULL, 1, 1, 1, 1, :matchBehavior, :searchBehavior, NULL)
      AND hidden = 0
      GROUP BY url
      ORDER BY ${orderBy}
      LIMIT ${limit > 0 ? limit : -1}`;
    if (this.#searchInProgress) {
      db.interrupt();
    }
    try {
      this.#searchInProgress = true;
      const rows = await db.executeCached(sql, {
        query,
        matchBehavior: Ci.mozIPlacesAutoComplete.MATCH_ANYWHERE_UNMODIFIED,
        searchBehavior: Ci.mozIPlacesAutoComplete.BEHAVIOR_HISTORY,
      });
      return rows.map(row => this.formatRowAsVisit(row));
    } finally {
      this.#searchInProgress = false;
    }
  }

  /**
   * Append a visit into the container it belongs to.
   *
   * @param {HistoryVisit} visit
   *   The visit to append.
   */
  #appendToCache(visit) {
    this.#getContainerForVisit(visit).push(visit);
    this.#insertIntoCachedHistoryPerUrl(visit);
  }

  /**
   * Insert a visit into the container it belongs to, ensuring to maintain
   * sorted order. Used for handling `page-visited` events after the initial
   * fetch of history data.
   *
   * @param {HistoryVisit} visit
   *   The visit to insert.
   */
  #insertSortedIntoCache(visit) {
    const container = this.#getContainerForVisit(visit);
    if (this.#cachedHistoryPerUrl.has(visit.url)) {
      const existingVisitsForUrl = this.#cachedHistoryPerUrl.get(visit.url);
      for (const existingVisit of existingVisitsForUrl) {
        if (this.#getContainerForVisit(existingVisit) === container) {
          if (existingVisit.date.getTime() >= visit.date.getTime()) {
            // Existing visit is more recent. Don't insert this one.
            return;
          }
          // Remove the existing visit, then insert the new one.
          container.splice(container.indexOf(existingVisit), 1);
          existingVisitsForUrl.delete(existingVisit);
          break;
        }
      }
    }

    let insertionPoint = 0;
    if (visit.date.getTime() < container[0]?.date.getTime()) {
      insertionPoint = lazy.BinarySearch.insertionIndexOf(
        (a, b) => b.date.getTime() - a.date.getTime(),
        container,
        visit
      );
    }
    container.splice(insertionPoint, 0, visit);
    this.#insertIntoCachedHistoryPerUrl(visit);
  }

  /**
   * Insert a visit into the url-keyed history cache.
   *
   * @param {HistoryVisit} visit
   *   The visit to insert.
   */
  #insertIntoCachedHistoryPerUrl(visit) {
    const container = this.#cachedHistoryPerUrl.get(visit.url);
    if (container) {
      container.add(visit);
    } else {
      this.#cachedHistoryPerUrl.set(visit.url, new Set().add(visit));
    }
  }

  /**
   * Retrieve the corresponding container for this visit.
   *
   * @param {HistoryVisit} visit
   *   The visit to check.
   * @returns {HistoryVisit[]}
   *   The container it belongs to.
   */
  #getContainerForVisit(visit) {
    switch (this.cachedHistoryOptions.sortBy) {
      case "datesite": {
        const dateKey = this.#getMapKeyForVisit(visit, "date");
        const siteKey = this.#getMapKeyForVisit(visit, "site");
        // @ts-expect-error - Bug 1966240
        if (!this.cachedHistory.has(dateKey)) {
          const siteContainer = [];
          // @ts-expect-error - Bug 1966240
          this.cachedHistory.set(dateKey, new Map([[siteKey, siteContainer]]));
          return siteContainer;
        }
        // @ts-expect-error - Bug 1966240
        const dateContainer = this.cachedHistory.get(dateKey);
        if (!dateContainer.has(siteKey)) {
          const siteContainer = [];
          dateContainer.set(siteKey, siteContainer);
          return siteContainer;
        }
        return dateContainer.get(siteKey);
      }
      case "lastvisited":
        // @ts-expect-error - Bug 1966240
        return this.cachedHistory;
      case "date":
      case "site":
      default: {
        const mapKey = this.#getMapKeyForVisit(visit);
        // @ts-expect-error - Bug 1966240
        let container = this.cachedHistory?.get(mapKey);
        if (!container) {
          container = [];
          // @ts-expect-error - Bug 1966240
          this.cachedHistory?.set(mapKey, container);
        }
        return container;
      }
    }
  }

  #getMapKeyForVisit(visit, sortBy = this.cachedHistoryOptions.sortBy) {
    switch (sortBy) {
      case "date":
        return this.getStartOfDayTimestamp(visit.date);
      case "site": {
        const { protocol } = new URL(visit.url);
        return protocol === "http:" || protocol === "https:"
          ? lazy.BrowserUtils.formatURIStringForDisplay(visit.url)
          : "";
      }
    }
    return null;
  }

  /**
   * Observe changes to the visits table. When changes are made, the callback
   * is given the new list of visits. Only one callback can be active at a time
   * (per instance). If one already exists, it will be replaced.
   *
   * @param {function(CachedHistory): any} callback
   *   The function to call when changes are made.
   */
  observeHistory(callback) {
    this.#historyListenerCallback = callback;
  }

  /**
   * Close this query. Caches are cleared and listeners are removed.
   */
  close() {
    this.#isClosed = true;
    this.cachedHistory = null;
    this.cachedHistoryOptions = null;
    this.#cachedHistoryPerUrl = null;
    if (this.#historyListener) {
      PlacesObservers.removeListener(
        [
          "page-removed",
          "page-visited",
          "history-cleared",
          "page-title-changed",
        ],
        this.#historyListener
      );
    }
    this.#historyListener = null;
    this.#historyListenerCallback = null;
    if (this.#historyObserverTask && !this.#historyObserverTask.isFinalized) {
      this.#historyObserverTask.disarm();
      this.#historyObserverTask.finalize();
    }
  }

  /**
   * Listen for changes to the visits table and update caches accordingly.
   */
  #initHistoryListener() {
    this.#historyObserverTask = new lazy.DeferredTask(
      async () => {
        if (typeof this.#historyListenerCallback === "function") {
          const history = await this.getHistory(this.cachedHistoryOptions);
          this.#historyListenerCallback(history);
        }
      },
      OBSERVER_DEBOUNCE_RATE_MS,
      OBSERVER_DEBOUNCE_TIMEOUT_MS
    );
    this.#historyListener = async events => {
      if (
        events.length >= BULK_PLACES_EVENTS_THRESHOLD ||
        events.some(({ type }) => type === "page-removed")
      ) {
        // Accounting for cascading deletes, or handling places events in bulk,
        // can be expensive. In this case, we invalidate the cache once rather
        // than handling each event individually.
        this.cachedHistory = null;
      } else if (this.cachedHistory != null) {
        for (const event of events) {
          switch (event.type) {
            case "page-visited":
              this.handlePageVisited(/** @type {PlacesVisit} */ (event));
              break;
            case "history-cleared":
              this.initializeCache();
              break;
            case "page-title-changed":
              this.handlePageTitleChanged(
                /** @type {PlacesVisitTitle} */ (event)
              );
              break;
          }
        }
      }
      this.#historyObserverTask.arm();
    };
    PlacesObservers.addListener(
      ["page-removed", "page-visited", "history-cleared", "page-title-changed"],
      this.#historyListener
    );
  }

  /**
   * Handle a page visited event.
   *
   * @param {PlacesVisit} event
   *   The event.
   * @returns {HistoryVisit}
   *   The visit that was inserted, or `null` if no visit was inserted.
   */
  handlePageVisited(event) {
    if (event.hidden) {
      return null;
    }
    const visit = this.formatEventAsVisit(event);
    this.#insertSortedIntoCache(visit);
    return visit;
  }

  /**
   * Handle a page title changed event.
   *
   * @param {PlacesVisitTitle} event
   *   The event.
   */
  handlePageTitleChanged(event) {
    const visits = this.#cachedHistoryPerUrl.get(event.url);
    if (visits == null) {
      return;
    }
    for (const visit of visits) {
      visit.title = event.title;
    }
  }

  /**
   * Get timestamp from a date by only considering its year, month, and date
   * (so that it can be used as a date-based key).
   *
   * @param {Date} date
   *   The date to truncate.
   * @returns {number}
   *   The corresponding timestamp.
   */
  getStartOfDayTimestamp(date) {
    return new Date(
      date.getFullYear(),
      date.getMonth(),
      date.getDate()
    ).getTime();
  }

  /**
   * Get timestamp from a date by only considering its year and month (so that
   * it can be used as a month-based key).
   *
   * @param {Date} date
   *   The date to truncate.
   * @returns {number}
   *   The corresponding timestamp.
   */
  getStartOfMonthTimestamp(date) {
    return new Date(date.getFullYear(), date.getMonth()).getTime();
  }

  /**
   * Format a database row as a history visit.
   *
   * @param {mozIStorageRow} row
   *   The row to format.
   * @returns {HistoryVisit}
   *   The resulting history visit.
   */
  formatRowAsVisit(row) {
    return {
      date: lazy.PlacesUtils.toDate(row.getResultByName("visit_date")),
      // @ts-expect-error - Bug 1966462
      title: row.getResultByName("title"),
      // @ts-expect-error - Bug 1966462
      url: row.getResultByName("url"),
      // @ts-expect-error - Bug 1966462
      guid: row.getResultByName("guid"),
    };
  }

  /**
   * Format a page visited event as a history visit.
   *
   * @param {PlacesVisit} event
   *   The event to format.
   * @returns {HistoryVisit}
   *   The resulting history visit.
   */
  formatEventAsVisit(event) {
    return {
      date: new Date(event.visitTime),
      title: event.lastKnownTitle,
      url: event.url,
      guid: event.pageGuid,
    };
  }
}
