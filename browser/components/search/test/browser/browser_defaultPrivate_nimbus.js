/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { ExperimentAPI } = ChromeUtils.importESModule(
  "resource://nimbus/ExperimentAPI.sys.mjs"
);
const { NimbusTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/NimbusTestUtils.sys.mjs"
);

ChromeUtils.defineESModuleGetters(this, {
  SearchTestUtils: "resource://testing-common/SearchTestUtils.sys.mjs",
});

const CONFIG_V2 = [
  {
    recordType: "engine",
    identifier: "basic",
    base: {
      name: "basic",
      urls: {
        search: {
          base: "https://example.com",
          searchTermParamName: "q",
        },
      },
    },
    variants: [{ environment: { allRegionsAndLocales: true } }],
  },
  {
    recordType: "engine",
    identifier: "private",
    base: {
      name: "private",
      urls: {
        search: {
          base: "https://example.com",
          searchTermParamName: "q",
        },
      },
    },
    variants: [{ environment: { allRegionsAndLocales: true } }],
  },
  {
    recordType: "defaultEngines",
    globalDefault: "basic",
    globalDefaultPrivate: "private",
    specificDefaults: [],
  },
  {
    recordType: "engineOrders",
    orders: [],
  },
];

SearchTestUtils.init(this);

add_setup(async () => {
  // Current default values.
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.search.separatePrivateDefault.ui.enabled", false],
      ["browser.search.separatePrivateDefault.urlbarResult.enabled", false],
      ["browser.search.separatePrivateDefault", true],
      ["browser.urlbar.suggest.searches", true],
    ],
  });

  await SearchTestUtils.updateRemoteSettingsConfig(CONFIG_V2);
});

add_task(async function test_nimbus_experiment() {
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "basic",
    "Should have basic as private default while not in experiment"
  );
  await ExperimentAPI.ready();

  let reloadObserved =
    SearchTestUtils.promiseSearchNotification("engines-reloaded");

  let doExperimentCleanup = await NimbusTestUtils.enrollWithFeatureConfig({
    featureId: "searchConfiguration",
    value: {
      separatePrivateDefaultUIEnabled: true,
      separatePrivateDefaultUrlbarResultEnabled: false,
      experiment: "testing",
    },
  });
  await reloadObserved;
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "private",
    "Should have private as private default while in experiment"
  );
  reloadObserved =
    SearchTestUtils.promiseSearchNotification("engines-reloaded");
  await doExperimentCleanup();
  await reloadObserved;
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "basic",
    "Should turn off private default and restore default engine after experiment"
  );
});

add_task(async function test_nimbus_experiment_urlbar_result_enabled() {
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "basic",
    "Should have basic as private default while not in experiment"
  );
  await ExperimentAPI.ready();

  let reloadObserved =
    SearchTestUtils.promiseSearchNotification("engines-reloaded");

  let doExperimentCleanup = await NimbusTestUtils.enrollWithFeatureConfig({
    featureId: "searchConfiguration",
    value: {
      separatePrivateDefaultUIEnabled: true,
      separatePrivateDefaultUrlbarResultEnabled: true,
      experiment: "testing",
    },
  });
  await reloadObserved;
  Assert.equal(
    Services.search.separatePrivateDefaultUrlbarResultEnabled,
    true,
    "Should have set the urlbar result enabled value to true"
  );
  reloadObserved =
    SearchTestUtils.promiseSearchNotification("engines-reloaded");
  await doExperimentCleanup();
  await reloadObserved;
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "basic",
    "Should turn off private default and restore default engine after experiment"
  );
});

add_task(async function test_non_experiment_prefs() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.search.separatePrivateDefault.ui.enabled", false]],
  });
  let uiPref = () =>
    Services.prefs.getBoolPref(
      "browser.search.separatePrivateDefault.ui.enabled"
    );
  Assert.equal(uiPref(), false, "defaulted false");
  await ExperimentAPI.ready();
  let doExperimentCleanup = await NimbusTestUtils.enrollWithFeatureConfig({
    featureId: "privatesearch",
    value: {
      separatePrivateDefaultUIEnabled: true,
    },
  });
  Assert.equal(uiPref(), false, "Pref did not change without experiment");
  await doExperimentCleanup();
  await SpecialPowers.popPrefEnv();
}).skip(); // The privatesearch feature does not exist.
