/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const ORIGINAL_URL = "https://example.com/document-builder.sjs?html=page1";
const OTHER_URL = "https://example.org/document-builder.sjs?html=page2";

/**
 * Starts waiting for the next URL change. Waits both for the URL to be updated
 * in the UI, but also for the navigation to done in the toolbox and all
 * requests to be settled.
 *
 * @param {string} url
 * @param {object} toolbox
 * @param {Tab} browserTab
 * @param {Window} win
 * @return {Function}
 *     Returns an async function that you can await to wait for the URL update
 *     to be fully processed.
 */
async function startWaitingForUrl(url, toolbox, browserTab, win) {
  const { onDomCompleteResource } =
    await waitForNextTopLevelDomCompleteResource(toolbox.commands);

  return async function () {
    info("Wait for URL");
    await waitUntil(
      () =>
        toolbox.target.url === url &&
        browserTab.linkedBrowser.currentURI.spec === url
    );

    info("Wait for dom complete");
    await onDomCompleteResource;

    info("Wait for toolbox requests to settle");
    await toolbox.commands.client.waitForRequestsToSettle();

    info("Wait for about debugging requests to settle");
    await waitForAboutDebuggingRequests(win.AboutDebugging.store);
  };
}

// Test that ensures the remote page can go forward and back via UI buttons
add_task(async function () {
  const browserTab = await addTab(ORIGINAL_URL);

  const { document, tab, window } = await openAboutDebugging();

  // go to This Firefox and inspect the new tab
  info("Inspecting a new tab in This Firefox");
  await selectThisFirefoxPage(document, window.AboutDebugging.store);
  const devToolsToolbox = await openAboutDevtoolsToolbox(
    document,
    tab,
    window,
    ORIGINAL_URL
  );
  const { devtoolsDocument, devtoolsWindow } = devToolsToolbox;
  const toolbox = getToolbox(devtoolsWindow);

  info("Navigating to another URL");
  let onTargetSwitched = toolbox.commands.targetCommand.once("switched-target");
  const urlInput = devtoolsDocument.querySelector(".devtools-textinput");
  let onURLReady = await startWaitingForUrl(
    OTHER_URL,
    toolbox,
    browserTab,
    window
  );
  await synthesizeUrlKeyInput(devToolsToolbox, urlInput, OTHER_URL);
  await onURLReady();
  await onTargetSwitched;

  info("Clicking back button");
  onTargetSwitched = toolbox.commands.targetCommand.once("switched-target");
  onURLReady = await startWaitingForUrl(
    ORIGINAL_URL,
    toolbox,
    browserTab,
    window
  );
  devtoolsDocument.querySelector(".qa-back-button").click();
  await onURLReady();
  await onTargetSwitched;

  info("Clicking the forward button");
  onTargetSwitched = toolbox.commands.targetCommand.once("switched-target");
  onURLReady = await startWaitingForUrl(OTHER_URL, toolbox, browserTab, window);
  devtoolsDocument.querySelector(".qa-forward-button").click();
  await onTargetSwitched;
  await onURLReady();

  ok(true, "Clicking back and forward works!");
});
