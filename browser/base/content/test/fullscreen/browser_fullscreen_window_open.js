/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

SimpleTest.requestLongerTimeout(2);

const IFRAME_ID = "testIframe";

async function testWindowOpen(iframeID) {
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, TEST_URL);
  info("Entering full-screen");
  await DOMFullscreenTestUtils.changeFullscreen(tab.linkedBrowser, true);

  let popup;
  await testExpectFullScreenExit(tab.linkedBrowser, true, async () => {
    info("Calling window.open()");
    popup = await jsWindowOpen(tab.linkedBrowser, true, iframeID);
  });

  // Cleanup
  await BrowserTestUtils.closeWindow(popup);
  BrowserTestUtils.removeTab(tab);
}

async function testWindowOpenExistingWindow(funToOpenExitingWindow, iframeID) {
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, TEST_URL);
  let popup = await jsWindowOpen(tab.linkedBrowser, true);

  info("re-focusing main window");
  await waitForFocus(tab.linkedBrowser);

  info("Entering full-screen");
  await DOMFullscreenTestUtils.changeFullscreen(tab.linkedBrowser, true);

  info("open existing popup window");
  await testExpectFullScreenExit(tab.linkedBrowser, true, async () => {
    await funToOpenExitingWindow(tab.linkedBrowser, iframeID);
  });

  // Cleanup
  await BrowserTestUtils.closeWindow(popup);
  BrowserTestUtils.removeTab(tab);
}

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["test.wait300msAfterTabSwitch", true],
      ["dom.disable_open_during_load", false], // Allow window.open calls without user interaction
      ["browser.link.open_newwindow.disabled_in_fullscreen", false],
    ],
  });
});

add_task(function test_parentWindowOpen() {
  return testWindowOpen();
});

add_task(function test_iframeWindowOpen() {
  return testWindowOpen(IFRAME_ID);
});

add_task(async function test_parentWindowOpenExistWindow() {
  await testWindowOpenExistingWindow(browser => {
    info(
      "Calling window.open() with same name again should reuse the existing window"
    );
    jsWindowOpen(browser, true);
  });
});

add_task(async function test_iframeWindowOpenExistWindow() {
  await testWindowOpenExistingWindow((browser, iframeID) => {
    info(
      "Calling window.open() with same name again should reuse the existing window"
    );
    jsWindowOpen(browser, true, iframeID);
  }, IFRAME_ID);
});

add_task(async function test_parentWindowClickLinkOpenExistWindow() {
  await testWindowOpenExistingWindow(browser => {
    info(
      "Clicking link with same target name should reuse the existing window"
    );
    jsClickLink(browser, true);
  });
});

add_task(async function test_iframeWindowClickLinkOpenExistWindow() {
  await testWindowOpenExistingWindow((browser, iframeID) => {
    info(
      "Clicking link with same target name should reuse the existing window"
    );
    jsClickLink(browser, true, iframeID);
  }, IFRAME_ID);
});
