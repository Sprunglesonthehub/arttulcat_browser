/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

let TalosParentProfiler;

export class TalosTabSwitchParent extends JSWindowActorParent {
  receiveMessage(message) {
    if (message.name == "tabswitch-do-test") {
      let browser = this.browsingContext.top.embedderElement;
      return this.test(browser.ownerGlobal);
    }

    return undefined;
  }

  /**
   * Returns a Promise that resolves when browser-delayed-startup-finished
   * fires for a given window
   *
   * @param win
   *        The window that we're waiting for the notification for.
   * @returns Promise
   */
  waitForDelayedStartup(win) {
    return new Promise(resolve => {
      const topic = "browser-delayed-startup-finished";
      Services.obs.addObserver(function onStartup(subject) {
        if (win == subject) {
          Services.obs.removeObserver(onStartup, topic);
          resolve();
        }
      }, topic);
    });
  }

  /**
   * For some <xul:tabbrowser>, loads a collection of URLs as new tabs
   * in that browser.
   *
   * @param gBrowser (<xul:tabbrowser>)
   *        The <xul:tabbrowser> in which to load the new tabs.
   * @param urls (Array)
   *        An array of URL strings to be loaded as new tabs.
   * @returns Promise
   *        Resolves once all tabs have finished loading.
   */
  loadTabs(gBrowser, urls) {
    return new Promise(resolve => {
      gBrowser.loadTabs(urls, {
        inBackground: true,
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      });

      let waitingToLoad = new Set(urls);

      let listener = {
        QueryInterface: ChromeUtils.generateQI([
          "nsIWebProgressListener",
          "nsISupportsWeakReference",
        ]),
        onStateChange(aBrowser, aWebProgress, aRequest, aStateFlags, aStatus) {
          let loadedState =
            Ci.nsIWebProgressListener.STATE_STOP |
            Ci.nsIWebProgressListener.STATE_IS_NETWORK;
          if (
            (aStateFlags & loadedState) == loadedState &&
            !aWebProgress.isLoadingDocument &&
            aWebProgress.isTopLevel &&
            Components.isSuccessCode(aStatus)
          ) {
            dump(`Loaded: ${aBrowser.currentURI.spec}\n`);
            waitingToLoad.delete(aBrowser.currentURI.spec);

            if (!waitingToLoad.size) {
              gBrowser.removeTabsProgressListener(listener);
              dump("Loads complete - starting tab switches\n");
              resolve();
            }
          }
        },
      };

      gBrowser.addTabsProgressListener(listener);
    });
  }

  /**
   * For some <xul:tab> in a browser window, have that window switch
   * to that tab. Returns a Promise that resolves ones the tab content
   * has been presented to the user.
   */
  async switchToTab(tab) {
    let browser = tab.linkedBrowser;
    let gBrowser = tab.ownerGlobal.gBrowser;

    let start = Cu.now();

    // We need to wait for the TabSwitchDone event to make sure
    // that the async tab switcher has shut itself down.
    let switchDone = this.waitForTabSwitchDone(browser);
    // Set up our promise that will wait for the content to be
    // presented.
    let finishPromise = this.waitForContentPresented(browser);
    // Finally, do the tab switch.
    gBrowser.selectedTab = tab;

    await switchDone;
    let finish = await finishPromise;

    return finish - start;
  }

  /**
   * For some <xul:browser>, find the <xul:tabbrowser> associated with it,
   * and wait until that tabbrowser has finished a tab switch. This function
   * assumes a tab switch has started, or is about to start.
   *
   * @param browser (<xul:browser>)
   *        The browser whose tabbrowser we expect to be involved in a tab
   *        switch.
   * @returns Promise
   *        Resolves once the TabSwitchDone event is fired.
   */
  waitForTabSwitchDone(browser) {
    return new Promise(resolve => {
      let gBrowser = browser.ownerGlobal.gBrowser;
      gBrowser.addEventListener(
        "TabSwitchDone",
        function onTabSwitchDone() {
          resolve();
        },
        { once: true }
      );
    });
  }

  /**
   * For some <xul:browser>, returns a Promise that resolves once its
   * content has been presented to the user.
   *
   * @param browser (<xul:browser>)
   *        The browser we expect to be presented.
   *
   * @returns Promise
   *        Resolves once the content has been presented. Resolves to
   *        the system time that the presentation occurred at, in
   *        milliseconds since midnight 01 January, 1970 UTC.
   */
  waitForContentPresented(browser) {
    return new Promise(resolve => {
      function onLayersReady() {
        let now = Cu.now();
        TalosParentProfiler.mark("Browser layers seen by tabswitch");
        resolve(now);
      }
      if (browser.hasLayers) {
        onLayersReady();
        return;
      }
      browser.addEventListener("MozLayerTreeReady", onLayersReady, {
        once: true,
      });
    });
  }

  /**
   * Do a garbage collect in the parent, and then a garbage
   * collection in the content process that the actor is
   * running in.
   *
   * @returns Promise
   *        Resolves once garbage collection has been completed in the
   *        parent, and the content process for the actor.
   */
  forceGC(win) {
    win.windowUtils.garbageCollect();
    return this.sendQuery("GarbageCollect");
  }

  /**
   * Given some host window, open a new window, browser its initial tab to
   * about:blank, then load up our set of testing URLs. Once they've all finished
   * loading, switch through each tab, recording their tab switch times. Finally,
   * report the results.
   *
   * @param window
   *        A host window. Primarily, we just use this for the OpenBrowserWindow
   *        function defined in that window.
   * @returns Promise
   */
  async test(window) {
    if (!window.gMultiProcessBrowser) {
      dump(
        "** The tabswitch Talos test does not support running in non-e10s mode " +
          "anymore! Bailing out!\n"
      );
      return null;
    }

    TalosParentProfiler = ChromeUtils.importESModule(
      "resource://talos-powers/TalosParentProfiler.sys.mjs"
    ).TalosParentProfiler;

    let testURLs = [];

    let win = window.OpenBrowserWindow();
    try {
      let prefFile = Services.prefs.getCharPref("addon.test.tabswitch.urlfile");
      if (prefFile) {
        testURLs = handleFile(win, prefFile);
      }
    } catch (ex) {
      /* error condition handled below */
    }
    if (!testURLs || !testURLs.length) {
      dump(
        "no tabs to test, 'addon.test.tabswitch.urlfile' pref isn't set to page set path\n"
      );
      return null;
    }

    await this.waitForDelayedStartup(win);

    let gBrowser = win.gBrowser;

    // We don't want to catch scrolling the tabstrip in our tests
    gBrowser.tabContainer.style.opacity = "0";

    let initialTab = gBrowser.selectedTab;
    await this.loadTabs(gBrowser, testURLs);

    // We'll switch back to about:blank after each tab switch
    // in an attempt to put the graphics layer into a "steady"
    // state before switching to the next tab.
    initialTab.linkedBrowser.loadURI(Services.io.newURI("about:blank"), {
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
        {}
      ),
    });

    let tabs = gBrowser.getTabsToTheEndFrom(initialTab);
    let times = [];

    for (let tab of tabs) {
      // Let's do an initial run to warm up any paint related caches
      // (like glyph caches for text). In the next loop we will start with
      // a GC before each switch so we don't need here.
      dump(`${tab.linkedBrowser.currentURI.spec}: warm up begin\n`);
      await this.switchToTab(tab);
      dump(`${tab.linkedBrowser.currentURI.spec}: warm up end\n`);

      await this.switchToTab(initialTab);
    }

    for (let tab of tabs) {
      // Moving a tab causes expensive style/layout computations on the tab bar
      // that are delayed using requestAnimationFrame, so wait for an animation
      // frame callback + one tick to ensure we aren't measuring the time it
      // takes to move a tab.
      gBrowser.moveTabTo(tab, { tabIndex: 1 });
      await new Promise(resolve => win.requestAnimationFrame(resolve));
      await new Promise(resolve => Services.tm.dispatchToMainThread(resolve));

      await this.forceGC(win);
      TalosParentProfiler.subtestStart();
      let time = await this.switchToTab(tab);
      TalosParentProfiler.subtestEnd(
        "TabSwitch Test: " + tab.linkedBrowser.currentURI.spec
      );
      dump(`${tab.linkedBrowser.currentURI.spec}: ${time}ms\n`);
      times.push(time);
      await this.switchToTab(initialTab);
    }

    let output =
      "<!DOCTYPE html>" +
      '<html lang="en">' +
      "<head><title>Tab Switch Results</title></head>" +
      "<body><h1>Tab switch times</h1>" +
      "<table>";
    let time = 0;
    for (let i in times) {
      time += times[i];
      output +=
        "<tr><td>" + testURLs[i] + "</td><td>" + times[i] + "ms</td></tr>";
    }
    output += "</table></body></html>";
    dump("total tab switch time:" + time + "\n");

    let resultsTab = win.gBrowser.addTab(
      "data:text/html;charset=utf-8," + encodeURIComponent(output),
      {
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      }
    );

    win.gBrowser.selectedTab = resultsTab;

    TalosParentProfiler.afterProfileGathered().then(() => {
      win.close();
    });

    return {
      times,
      urls: testURLs,
    };
  }
}

// This just has to match up with the make_talos_domain function in talos.py
function makeTalosDomain(host) {
  return host + "-talos";
}

function handleFile(win, file) {
  let localFile = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
  localFile.initWithPath(file);
  let localURI = Services.io.newFileURI(localFile);
  let req = new win.XMLHttpRequest();
  req.open("get", localURI.spec, false);
  req.send(null);

  let testURLs = [];
  let maxurls = Services.prefs.getIntPref("addon.test.tabswitch.maxurls");
  let lines = req.responseText.split('<a href="');
  testURLs = [];
  if (maxurls && maxurls > 0) {
    lines.splice(maxurls, lines.length);
  }
  lines.forEach(function (a) {
    let url = a.split('"')[0];
    if (url != "") {
      let domain = url.split("/")[0];
      if (domain != "") {
        testURLs.push(`http://${makeTalosDomain(domain)}/fis/tp5n/${url}`);
      }
    }
  });

  return testURLs;
}
