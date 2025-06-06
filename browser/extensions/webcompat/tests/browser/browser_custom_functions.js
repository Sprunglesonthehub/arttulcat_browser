"use strict";

add_setup(async function () {
  // We don't send events or call official addon APIs while running
  // these tests, so there a good chance that test-verify mode may
  // end up seeing the addon as "idle". This pref should avoid that.
  await SpecialPowers.pushPrefEnv({
    set: [["extensions.background.idle.timeout", 300_000]],
  });
});

async function setupTestIntervention(interventions) {
  const config = {
    id: "bugnumber_test",
    label: "test intervention",
    bugs: {
      issue1: {
        matches: ["*://example.com/*"],
      },
    },
    interventions: interventions.map(i =>
      Object.assign({ platforms: ["all"] }, i)
    ),
  };

  const results = await WebCompatExtension.updateInterventions([config]);
  ok(results[0].active, "Verify intervention is active");
}

async function testResponseHeaderValue({
  test,
  browser,
  serverSends,
  expect,
  useServer,
}) {
  let server = useServer ?? "https://example.com";
  const results = await ContentTask.spawn(
    browser,
    { server, serverSends, expect },
    async function (args) {
      const send = JSON.stringify(Object.entries(args.serverSends ?? {}));
      const url = `${args.server}/browser/browser/extensions/webcompat/tests/browser/download_server.sjs?${send}`;
      const { headers } = await content.wrappedJSObject.fetch(url);
      return Object.fromEntries(
        Object.keys(args.expect).map(name => [name, headers.get(name)])
      );
    }
  );
  for (const [name, expected] of Object.entries(expect)) {
    is(results[name], expected, `${test}, for header ${name}`);
  }
}

async function getRequestHeaders(browser, alter_request_headers) {
  await setupTestIntervention([{ alter_request_headers }]);

  let url =
    "https://example.com/browser/browser/extensions/webcompat/tests/browser/echo_headers.sjs";
  let loaded = BrowserTestUtils.browserLoaded(browser, true, url, true);
  BrowserTestUtils.startLoadingURIString(browser, url);
  await loaded;

  const headers = await SpecialPowers.spawn(browser, [], async () => {
    return content.wrappedJSObject.headers;
  });

  return headers;
}

add_task(async function test_alter_response_headers() {
  await WebCompatExtension.started();

  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser);
  let browser = tab.linkedBrowser;

  let headers = await getRequestHeaders(browser, [
    {
      headers: ["user-agent"],
      replacement: "Test",
    },
  ]);
  is(headers["user-agent"], "Test", "Test basic replacement");

  headers = await getRequestHeaders(browser, [
    {
      headers: ["user-agent"],
      replacement: null,
    },
  ]);
  is(headers["user-agent"], undefined, "Test removal");

  headers = await getRequestHeaders(browser, [
    {
      headers: ["user-agent"],
      replace: "certainly won't match",
      replacement: null,
    },
  ]);
  is(headers["user-agent"], undefined, "Test selective removal");

  headers = await getRequestHeaders(browser, [
    {
      headers: ["user-agent"],
      replace: "(Firefox)",
      replacement: "Special$1Change",
    },
  ]);
  is(
    headers["user-agent"],
    navigator.userAgent.replace("Firefox", "SpecialFirefoxChange"),
    "Test regexp replacement"
  );

  headers = await getRequestHeaders(browser, [
    {
      headers: ["unknown"],
      fallback: "fallback",
    },
  ]);
  is(headers.unknown, "fallback", "Test fallback");

  headers = await getRequestHeaders(browser, [
    {
      headers: ["user-agent"],
      fallback: "fallback",
    },
  ]);
  is(
    headers["user-agent"],
    navigator.userAgent,
    "Test fallback not used if found"
  );

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_alter_response_headers() {
  await WebCompatExtension.started();

  const tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    "https://example.com"
  );
  const browser = gBrowser.selectedBrowser;

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["content-disposition"],
          replace: "filename\\*=UTF-8''([^;]+)",
          replacement: 'filename="$1"',
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that `replace` and `replacement` work as regexes",
    browser,
    serverSends: {
      "content-disposition": "attachment; filename*=UTF-8''rj.txt",
    },
    expect: { "content-disposition": 'attachment; filename="rj.txt"' },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["sent-header"],
          replace: "test",
          replacement: "test2",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that `replace` and `replacement` work on all matches",
    browser,
    serverSends: { "sent-header": "test test test" },
    expect: { "sent-header": "test2 test2 test2" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["sent-header1", "sent-header2", "unsent-header"],
          replace: "test",
          replacement: "test2",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that every specified header is altered if sent by the server",
    browser,
    serverSends: { "sent-header1": "test test", "sent-header2": "test test" },
    expect: {
      "sent-header1": "test2 test2",
      "unsent-header": null,
    },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["sent-header"],
          replacement: "good",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that if `replace` is not specified, `replacement` is used instead of what the server sends",
    browser,
    serverSends: { "sent-header": "bad" },
    expect: { "sent-header": "good" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["sent-header"],
          replacement: null,
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that setting `replacement` to `null` removes a header",
    browser,
    serverSends: {
      "sent-header": "bad",
    },
    expect: { "sent-header": null },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["unsent-header"],
          replacement: "good",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that if `replace` is not specified, `replacement` is used even if the server doesn't send a value for that header",
    browser,
    serverSends: {},
    expect: { "unsent-header": "good" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["sent-header"],
          replace: "xxxx",
          replacement: "test",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that `replace` does not replace the value if the regexp does not match",
    browser,
    serverSends: { "sent-header": "yyyy" },
    expect: { "sent-header": "yyyy" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["unsent-header"],
          replace: "^.*$",
          replacement: "test",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that if `replace` is given but no such header is sent, it's still left as unsent",
    browser,
    serverSends: {},
    expect: { "unsent-header": null },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          headers: ["unsent-header", "unsent-header2"],
          replace: "^.*$",
          replacement: "bad",
          fallback: "good",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that altering a response header which isn't sent results in the fallback value used for the first one",
    browser,
    serverSends: {},
    expect: { "unsent-header": "good" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          urls: ["https://example.net/*"],
          headers: ["sent-header"],
          replacement: "good",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that `urls` overrides the `matches` on the intervention",
    browser,
    useServer: "https://example.net",
    serverSends: { "sent-header": "bad" },
    expect: { "sent-header": "good" },
  });

  await setupTestIntervention([
    {
      alter_response_headers: [
        {
          urls: ["https://example.net/*"],
          headers: ["sent-header"],
          replacement: "shouldChangeAgain",
        },
      ],
    },
    {
      alter_response_headers: [
        {
          urls: ["https://example.com/*"],
          headers: ["sent-header"],
          replacement: "shouldNotHappen",
        },
      ],
    },
    {
      alter_response_headers: [
        {
          urls: ["https://example.net/*"],
          headers: ["sent-header"],
          replacement: "good",
        },
      ],
    },
  ]);
  await testResponseHeaderValue({
    test: "verify that multiple alter_response_headers work on the correct URLs",
    browser,
    useServer: "https://example.net",
    serverSends: { "sent-header": "unchanged" },
    expect: { "sent-header": "good" },
  });

  BrowserTestUtils.removeTab(tab);
});
