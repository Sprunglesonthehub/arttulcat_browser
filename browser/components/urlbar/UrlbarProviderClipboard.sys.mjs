/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  UrlbarProvider,
  UrlbarUtils,
} from "resource:///modules/UrlbarUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  UrlbarResult: "resource:///modules/UrlbarResult.sys.mjs",
  UrlbarPrefs: "resource:///modules/UrlbarPrefs.sys.mjs",
  UrlbarTokenizer: "resource:///modules/UrlbarTokenizer.sys.mjs",
});

const RESULT_MENU_COMMANDS = {
  DISMISS: "dismiss",
};
const CLIPBOARD_IMPRESSION_LIMIT = 2;

/**
 * A provider that returns a suggested url to the user based
 * on a valid URL stored in the clipboard.
 */
class ProviderClipboard extends UrlbarProvider {
  #previousClipboard = {
    value: "",
    impressionsLeft: CLIPBOARD_IMPRESSION_LIMIT,
  };

  constructor() {
    super();
  }

  get name() {
    return "UrlbarProviderClipboard";
  }

  /**
   * @returns {Values<typeof UrlbarUtils.PROVIDER_TYPE>}
   */
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }

  setPreviousClipboardValue(newValue) {
    this.#previousClipboard.value = newValue;
  }

  async isActive(queryContext, controller) {
    // Return clipboard results only for empty searches.
    if (
      !lazy.UrlbarPrefs.get("clipboard.featureGate") ||
      !lazy.UrlbarPrefs.get("suggest.clipboard") ||
      queryContext.searchString ||
      queryContext.searchMode
    ) {
      return false;
    }
    let textFromClipboard = controller.browserWindow.readFromClipboard();

    // Check for spaces in clipboard text to avoid suggesting
    // clipboard content including both a url and the following text.
    if (
      !textFromClipboard ||
      textFromClipboard.length > 2048 ||
      lazy.UrlbarTokenizer.REGEXP_SPACES.test(textFromClipboard)
    ) {
      return false;
    }
    textFromClipboard =
      controller.input.sanitizeTextFromClipboard(textFromClipboard);
    const validUrl = this.#validUrl(textFromClipboard);
    if (!validUrl) {
      return false;
    }

    if (this.#previousClipboard.value === validUrl) {
      if (this.#previousClipboard.impressionsLeft <= 0) {
        return false;
      }
    } else {
      this.#previousClipboard = {
        value: validUrl,
        impressionsLeft: CLIPBOARD_IMPRESSION_LIMIT,
      };
    }

    return true;
  }

  #validUrl(clipboardVal) {
    let givenUrl = URL.parse(clipboardVal);
    if (!givenUrl) {
      // Not a valid URI.
      return null;
    }

    if (givenUrl.protocol == "http:" || givenUrl.protocol == "https:") {
      return givenUrl.href;
    }

    return null;
  }

  getPriority() {
    // Zero-prefix suggestions have the same priority as top sites.
    return 1;
  }

  async startQuery(queryContext, addCallback) {
    // If the query was started, isActive should have cached a url already.
    let result = new lazy.UrlbarResult(
      UrlbarUtils.RESULT_TYPE.URL,
      UrlbarUtils.RESULT_SOURCE.OTHER_LOCAL,
      ...lazy.UrlbarResult.payloadAndSimpleHighlights(queryContext.tokens, {
        fallbackTitle: [
          UrlbarUtils.prepareUrlForDisplay(this.#previousClipboard.value, {
            trimURL: false,
          }),
          UrlbarUtils.HIGHLIGHT.NONE,
        ],
        url: [this.#previousClipboard.value, UrlbarUtils.HIGHLIGHT.NONE],
        icon: "chrome://global/skin/icons/clipboard.svg",
        isBlockable: true,
      })
    );

    addCallback(this, result);
  }

  onEngagement(queryContext, controller, details) {
    this.#previousClipboard.impressionsLeft = 0; // User has picked the suggested clipboard result
    // Handle commands.
    this.#handlePossibleCommand(
      controller.view,
      details.result,
      details.selType
    );
  }

  onImpression() {
    this.#previousClipboard.impressionsLeft--;
  }

  #handlePossibleCommand(view, result, selType) {
    switch (selType) {
      case RESULT_MENU_COMMANDS.DISMISS:
        view.controller.removeResult(result);
        this.#previousClipboard.impressionsLeft = 0;
        break;
    }
  }
}

const UrlbarProviderClipboard = new ProviderClipboard();
export { UrlbarProviderClipboard, CLIPBOARD_IMPRESSION_LIMIT };
