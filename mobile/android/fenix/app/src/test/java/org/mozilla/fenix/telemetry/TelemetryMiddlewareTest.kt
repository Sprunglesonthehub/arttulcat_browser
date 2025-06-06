/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.telemetry

import androidx.test.core.app.ApplicationProvider
import io.mockk.every
import io.mockk.just
import io.mockk.mockk
import io.mockk.runs
import io.mockk.verify
import mozilla.components.browser.state.action.ContentAction
import mozilla.components.browser.state.action.EngineAction
import mozilla.components.browser.state.action.ExtensionsProcessAction
import mozilla.components.browser.state.action.TabListAction
import mozilla.components.browser.state.action.TranslationsAction
import mozilla.components.browser.state.engine.EngineMiddleware
import mozilla.components.browser.state.state.BrowserState
import mozilla.components.browser.state.state.createTab
import mozilla.components.browser.state.state.recover.RecoverableTab
import mozilla.components.browser.state.state.recover.TabState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.engine.Engine
import mozilla.components.concept.engine.translate.TranslationError
import mozilla.components.concept.engine.translate.TranslationOperation
import mozilla.components.support.base.android.Clock
import mozilla.components.support.test.ext.joinBlocking
import mozilla.components.support.test.robolectric.testContext
import mozilla.components.support.test.rule.MainCoroutineRule
import mozilla.telemetry.glean.internal.TimerId
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.GleanMetrics.Addons
import org.mozilla.fenix.GleanMetrics.Events
import org.mozilla.fenix.GleanMetrics.Metrics
import org.mozilla.fenix.GleanMetrics.Translations
import org.mozilla.fenix.components.AppStore
import org.mozilla.fenix.components.appstate.AppAction
import org.mozilla.fenix.components.metrics.Event
import org.mozilla.fenix.components.metrics.MetricController
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.helpers.FenixGleanTestRule
import org.mozilla.fenix.utils.Settings
import org.robolectric.RobolectricTestRunner
import org.robolectric.shadows.ShadowLooper
import org.mozilla.fenix.GleanMetrics.EngineTab as EngineMetrics

private const val SEARCH_ENGINE_NAME = "Test"

@RunWith(RobolectricTestRunner::class)
class TelemetryMiddlewareTest {

    private lateinit var store: BrowserStore
    private lateinit var appStore: AppStore
    private lateinit var settings: Settings
    private lateinit var telemetryMiddleware: TelemetryMiddleware

    @get:Rule
    val coroutinesTestRule = MainCoroutineRule()

    @get:Rule
    val gleanRule = FenixGleanTestRule(ApplicationProvider.getApplicationContext())

    private val clock = FakeClock()
    private val metrics: MetricController = mockk()
    private val searchState: MutableMap<String, TimerId> = mutableMapOf()
    private val timerId = Metrics.searchPageLoadTime.start()

    @Before
    fun setUp() {
        Clock.delegate = clock
        settings = Settings(testContext)
        telemetryMiddleware = TelemetryMiddleware(
            context = testContext,
            settings = settings,
            metrics = metrics,
            nimbusSearchEngine = SEARCH_ENGINE_NAME,
            searchState = searchState,
            timerId = timerId,
        )
        val engine: Engine = mockk()
        every { engine.enableExtensionProcessSpawning() } just runs
        every { engine.disableExtensionProcessSpawning() } just runs
        every { engine.getSupportedTranslationLanguages(any(), any()) } just runs
        every { engine.isTranslationsEngineSupported(any(), any()) } just runs
        every { engine.createSession(any(), any()) } returns mockk(relaxed = true)

        store = BrowserStore(
            middleware = listOf(telemetryMiddleware) + EngineMiddleware.create(engine),
            initialState = BrowserState(),
        )
        appStore = AppStore()
        every { testContext.components.appStore } returns appStore
    }

    @After
    fun tearDown() {
        Clock.reset()
    }

    @Test
    fun `WHEN action is UpdateIsSearchAction & all valid args THEN searchState is updated with session id and timer id`() {
        assertTrue(searchState.isEmpty())

        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, true, SEARCH_ENGINE_NAME))
            .joinBlocking()

        assertEquals(1, searchState.size)
        assertEquals(mutableMapOf(sessionId to timerId), searchState)
    }

    @Test
    fun `WHEN action is UpdateIsSearchAction & action is not search THEN searchState is not updated`() {
        assertTrue(searchState.isEmpty())

        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, false, SEARCH_ENGINE_NAME))
            .joinBlocking()

        assertTrue(searchState.isEmpty())
    }

    @Test
    fun `WHEN action is UpdateIsSearchAction & search engine name is empty THEN searchState is not updated`() {
        assertTrue(searchState.isEmpty())

        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, true, ""))
            .joinBlocking()

        assertTrue(searchState.isEmpty())
    }

    @Test
    fun `WHEN action is UpdateIsSearchAction & search engine name is different to Nimbus THEN searchState is not updated`() {
        assertTrue(searchState.isEmpty())

        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, true, "$SEARCH_ENGINE_NAME 2"))
            .joinBlocking()

        assertTrue(searchState.isEmpty())
    }

    @Test
    fun `WHEN action is UpdateLoadingStateAction & progress completed THEN telemetry is added & searchState is empty`() {
        assertNull(Metrics.searchPageLoadTime.testGetValue())

        // Start searchState
        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, true, SEARCH_ENGINE_NAME))
            .joinBlocking()

        assertEquals(1, searchState.size)
        assertEquals(mutableMapOf(sessionId to timerId), searchState)

        // Update hasFinishedLoading
        val tab = createTab(
            id = sessionId,
            url = "https://mozilla.org",
        ).let { it.copy(content = it.content.copy(progress = 100)) }

        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, true)).joinBlocking()
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, false)).joinBlocking()
        val count = Events.normalAndPrivateUriCount.testGetValue()!!
        assertEquals(1, count)

        // Finish searchState
        assertNotNull(Metrics.searchPageLoadTime.testGetValue())
        assertTrue(searchState.isEmpty())
    }

    @Test
    fun `WHEN action is UpdateLoadingStateAction & progress not completed THEN no telemetry & searchState is empty`() {
        assertNull(Metrics.searchPageLoadTime.testGetValue())

        // Start searchState
        val sessionId = "1235"
        store.dispatch(ContentAction.UpdateIsSearchAction(sessionId, true, SEARCH_ENGINE_NAME))
            .joinBlocking()

        assertEquals(1, searchState.size)
        assertEquals(mutableMapOf(sessionId to timerId), searchState)

        // Update hasFinishedLoading
        val tab = createTab(
            id = sessionId,
            url = "https://mozilla.org",
        ).let { it.copy(content = it.content.copy(progress = 50)) }

        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, true)).joinBlocking()
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, false)).joinBlocking()
        val count = Events.normalAndPrivateUriCount.testGetValue()!!
        assertEquals(1, count)

        // Finish searchState
        assertNull(Metrics.searchPageLoadTime.testGetValue())
        assertTrue(searchState.isEmpty())
    }

    @Test
    fun `WHEN a tab is added THEN the open tab count is updated`() {
        assertEquals(0, settings.openTabsCount)
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(TabListAction.AddTabAction(createTab("https://mozilla.org"))).joinBlocking()
        assertEquals(1, settings.openTabsCount)

        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN a private tab is added THEN the open tab count is not updated`() {
        assertEquals(0, settings.openTabsCount)
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(TabListAction.AddTabAction(createTab("https://mozilla.org", private = true))).joinBlocking()
        assertEquals(0, settings.openTabsCount)

        assertFalse(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN multiple tabs are added THEN the open tab count is updated`() {
        assertEquals(0, settings.openTabsCount)
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(
            TabListAction.AddMultipleTabsAction(
                listOf(
                    createTab("https://mozilla.org"),
                    createTab("https://firefox.com"),
                ),
            ),
        ).joinBlocking()

        assertEquals(2, settings.openTabsCount)

        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN a tab is removed THEN the open tab count is updated`() {
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(
            TabListAction.AddMultipleTabsAction(
                listOf(
                    createTab(id = "1", url = "https://mozilla.org"),
                    createTab(id = "2", url = "https://firefox.com"),
                ),
            ),
        ).joinBlocking()
        assertEquals(2, settings.openTabsCount)

        store.dispatch(TabListAction.RemoveTabAction("1")).joinBlocking()
        assertEquals(1, settings.openTabsCount)

        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN all tabs are removed THEN the open tab count is updated`() {
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(
            TabListAction.AddMultipleTabsAction(
                listOf(
                    createTab("https://mozilla.org"),
                    createTab("https://firefox.com"),
                ),
            ),
        ).joinBlocking()
        assertEquals(2, settings.openTabsCount)

        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)

        store.dispatch(TabListAction.RemoveAllTabsAction()).joinBlocking()
        assertEquals(0, settings.openTabsCount)

        assertFalse(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN all normal tabs are removed THEN the open tab count is updated`() {
        assertNull(Metrics.hasOpenTabs.testGetValue())

        store.dispatch(
            TabListAction.AddMultipleTabsAction(
                listOf(
                    createTab("https://mozilla.org"),
                    createTab("https://firefox.com"),
                    createTab("https://getpocket.com", private = true),
                ),
            ),
        ).joinBlocking()
        assertEquals(2, settings.openTabsCount)
        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)

        store.dispatch(TabListAction.RemoveAllNormalTabsAction).joinBlocking()
        assertEquals(0, settings.openTabsCount)
        assertFalse(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `WHEN tabs are restored THEN the open tab count is updated`() {
        assertEquals(0, settings.openTabsCount)
        assertNull(Metrics.hasOpenTabs.testGetValue())

        val tabsToRestore = listOf(
            RecoverableTab(null, TabState(url = "https://mozilla.org", id = "1")),
            RecoverableTab(null, TabState(url = "https://firefox.com", id = "2")),
        )

        store.dispatch(
            TabListAction.RestoreAction(
                tabs = tabsToRestore,
                restoreLocation = TabListAction.RestoreAction.RestoreLocation.BEGINNING,
            ),
        ).joinBlocking()
        assertEquals(2, settings.openTabsCount)

        assertTrue(Metrics.hasOpenTabs.testGetValue()!!)
    }

    @Test
    fun `GIVEN a normal page is loading WHEN loading is complete THEN we record a UriOpened event`() {
        val tab = createTab(id = "1", url = "https://mozilla.org")
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, true)).joinBlocking()
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, false)).joinBlocking()
        val count = Events.normalAndPrivateUriCount.testGetValue()!!
        assertEquals(1, count)
    }

    @Test
    fun `GIVEN a private page is loading WHEN loading is complete THEN we record a UriOpened event`() {
        val tab = createTab(id = "1", url = "https://mozilla.org", private = true)
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, true)).joinBlocking()
        assertNull(Events.normalAndPrivateUriCount.testGetValue())

        store.dispatch(ContentAction.UpdateLoadingStateAction(tab.id, false)).joinBlocking()
        val count = Events.normalAndPrivateUriCount.testGetValue()!!
        assertEquals(1, count)
    }

    @Test
    fun `WHEN tabs gets killed THEN middleware sends an event`() {
        store.dispatch(
            TabListAction.RestoreAction(
                listOf(
                    RecoverableTab(null, TabState(url = "https://www.mozilla.org", id = "foreground")),
                    RecoverableTab(null, TabState(url = "https://getpocket.com", id = "background_pocket", hasFormData = true)),
                ),
                selectedTabId = "foreground",
                restoreLocation = TabListAction.RestoreAction.RestoreLocation.BEGINNING,
            ),
        ).joinBlocking()

        assertNull(EngineMetrics.tabKilled.testGetValue())

        store.dispatch(
            EngineAction.KillEngineSessionAction("background_pocket"),
        ).joinBlocking()

        assertEquals(1, EngineMetrics.tabKilled.testGetValue()?.size)
        EngineMetrics.tabKilled.testGetValue()?.get(0)?.extra?.also {
            assertEquals("false", it["foreground_tab"])
            assertEquals("true", it["had_form_data"])
            assertEquals("true", it["app_foreground"])
        }

        appStore.dispatch(
            AppAction.AppLifecycleAction.PauseAction,
        ).joinBlocking()

        store.dispatch(
            EngineAction.KillEngineSessionAction("foreground"),
        ).joinBlocking()

        assertEquals(2, EngineMetrics.tabKilled.testGetValue()?.size)
        EngineMetrics.tabKilled.testGetValue()?.get(1)?.extra?.also {
            assertEquals("true", it["foreground_tab"])
            assertEquals("false", it["had_form_data"])
            assertEquals("false", it["app_foreground"])
        }
    }

    @Test
    fun `GIVEN the request to check for form data WHEN it fails THEN telemetry is sent`() {
        assertNull(Events.formDataFailure.testGetValue())

        store.dispatch(
            ContentAction.CheckForFormDataExceptionAction("1", RuntimeException("session form data request failed")),
        ).joinBlocking()

        // Wait for the main looper to process the re-thrown exception.
        ShadowLooper.idleMainLooper()

        assertNotNull(Events.formDataFailure.testGetValue())
    }

    @Test
    fun `GIVEN an existing tab WHEN it reloads THEN telemetry is sent`() {
        val tabId = "test-tab-id"

        store.dispatch(TabListAction.AddTabAction(createTab(id = tabId, url = "https://firefox.com")))
            .joinBlocking()

        store.dispatch(
            EngineAction.KillEngineSessionAction(tabId),
        ).joinBlocking()
        assertTrue(store.state.recentlyKilledTabs.contains(tabId))

        store.dispatch(
            EngineAction.CreateEngineSessionAction(tabId),
        ).joinBlocking()

        ShadowLooper.idleMainLooper()

        val recordedEvents = EngineMetrics.reloaded.testGetValue()
        assertNotNull(recordedEvents)
        assertEquals(1, recordedEvents!!.size)

        assertFalse(store.state.recentlyKilledTabs.contains(tabId))
    }

    @Test
    fun `GIVEN a tab that was not recently killed WHEN it reloads THEN telemetry is NOT sent`() {
        val tabId = "test-tab-id"

        store.dispatch(
            TabListAction.AddTabAction(createTab(id = tabId, url = "https://firefox.com")),
        ).joinBlocking()

        store.dispatch(
            EngineAction.CreateEngineSessionAction(tabId),
        ).joinBlocking()

        ShadowLooper.idleMainLooper()

        val recordedEvents = EngineMetrics.reloaded.testGetValue()
        assertTrue(recordedEvents.isNullOrEmpty())
    }

    @Test
    fun `GIVEN a tab that is killed multiple times WHEN checking recentlyKilledTabs THEN it only appears once`() {
        val tabId = "test-tab-id"

        store.dispatch(EngineAction.KillEngineSessionAction(tabId)).joinBlocking()
        store.dispatch(EngineAction.KillEngineSessionAction(tabId)).joinBlocking()

        assertEquals(1, store.state.recentlyKilledTabs.count { it == tabId })
    }

    @Test
    fun `GIVEN more than 50 tabs are killed WHEN checking recentlyKilledTabs THEN it does not exceed 50`() {
        repeat(51) { i ->
            store.dispatch(EngineAction.KillEngineSessionAction("tab-$i")).joinBlocking()
        }

        assertEquals(50, store.state.recentlyKilledTabs.size)
    }

    @Test
    fun `GIVEN 50 killed tabs WHEN another killed tab is reloaded THEN oldest tab is removed and reloaded tab is recorded`() {
        val oldestTabId = "tab-id-0"
        val newTabId = "new-tab-id"

        // Fill recentlyKilledTabs with 50 entries and verify max limit is reached
        repeat(50) { i ->
            val tabId = "tab-id-$i"
            store.dispatch(TabListAction.AddTabAction(createTab(id = tabId, url = "https://example.com/$i"))).joinBlocking()
            store.dispatch(EngineAction.KillEngineSessionAction(tabId)).joinBlocking()
        }
        assertTrue(store.state.recentlyKilledTabs.contains(oldestTabId))
        assertEquals(50, store.state.recentlyKilledTabs.size)

        // Kill one more tab and verify oldest tab is removed
        store.dispatch(TabListAction.AddTabAction(createTab(id = newTabId, url = "https://example.com/$newTabId"))).joinBlocking()
        store.dispatch(EngineAction.KillEngineSessionAction(newTabId)).joinBlocking()
        assertFalse(store.state.recentlyKilledTabs.contains(oldestTabId))
        assertTrue(store.state.recentlyKilledTabs.contains(newTabId))
        assertEquals(50, store.state.recentlyKilledTabs.size)

        // Verify the reload of the newest tab was recorded
        val recordedEventsBefore = EngineMetrics.reloaded.testGetValue()?.size ?: 0
        store.dispatch(EngineAction.CreateEngineSessionAction(newTabId)).joinBlocking()
        ShadowLooper.idleMainLooper()
        val recordedEventsAfter = EngineMetrics.reloaded.testGetValue()
        assertNotNull(recordedEventsAfter)
        assertEquals(recordedEventsBefore + 1, recordedEventsAfter!!.size)
    }

    @Test
    fun `WHEN uri loaded to engine THEN matching event is sent to metrics`() {
        store.dispatch(EngineAction.LoadUrlAction("", "")).joinBlocking()

        verify { metrics.track(Event.GrowthData.FirstUriLoadForDay) }
    }

    @Test
    fun `WHEN EnabledAction is dispatched THEN enable the process spawning`() {
        assertNull(Addons.extensionsProcessUiRetry.testGetValue())
        assertNull(Addons.extensionsProcessUiDisable.testGetValue())

        store.dispatch(ExtensionsProcessAction.EnabledAction).joinBlocking()

        assertEquals(1, Addons.extensionsProcessUiRetry.testGetValue())
        assertNull(Addons.extensionsProcessUiDisable.testGetValue())
    }

    @Test
    fun `WHEN DisabledAction is dispatched THEN disable the process spawning`() {
        assertNull(Addons.extensionsProcessUiRetry.testGetValue())
        assertNull(Addons.extensionsProcessUiDisable.testGetValue())

        store.dispatch(ExtensionsProcessAction.DisabledAction).joinBlocking()

        assertEquals(1, Addons.extensionsProcessUiDisable.testGetValue())
        assertNull(Addons.extensionsProcessUiRetry.testGetValue())
    }

    @Test
    fun `WHEN TranslateOfferAction is dispatched THEN update telemetry`() {
        assertNull(Translations.offerEvent.testGetValue())

        store.dispatch(TranslationsAction.TranslateOfferAction(tabId = "1", true)).joinBlocking()

        val telemetry = Translations.offerEvent.testGetValue()?.firstOrNull()
        assertEquals("offer", telemetry?.extra?.get("item"))
    }

    @Test
    fun `WHEN TranslateExpectedAction is dispatched THEN update telemetry`() {
        assertNull(Translations.offerEvent.testGetValue())

        store.dispatch(TranslationsAction.TranslateExpectedAction(tabId = "1")).joinBlocking()

        val telemetry = Translations.offerEvent.testGetValue()?.firstOrNull()
        assertEquals("expected", telemetry?.extra?.get("item"))
    }

    @Test
    fun `WHEN TranslateAction is dispatched THEN update telemetry`() {
        assertNull(Translations.translateRequested.testGetValue())

        store.dispatch(
            TranslationsAction.TranslateAction(
                tabId = "1",
                fromLanguage = "en",
                toLanguage = "es",
                options = null,
            ),
        ).joinBlocking()

        val telemetry = Translations.translateRequested.testGetValue()?.firstOrNull()
        assertEquals("es", telemetry?.extra?.get("to_language"))
        assertEquals("en", telemetry?.extra?.get("from_language"))
    }

    @Test
    fun `WHEN TranslateSuccessAction is dispatched THEN update telemetry`() {
        assertNull(Translations.translateSuccess.testGetValue())

        // Shouldn't record other operations
        store.dispatch(
            TranslationsAction.TranslateSuccessAction(
                tabId = "1",
                operation = TranslationOperation.FETCH_SUPPORTED_LANGUAGES,
            ),
        ).joinBlocking()
        assertNull(Translations.translateSuccess.testGetValue())

        // Should record translate operations
        store.dispatch(
            TranslationsAction.TranslateSuccessAction(
                tabId = "1",
                operation = TranslationOperation.TRANSLATE,
            ),
        ).joinBlocking()

        val telemetry = Translations.translateSuccess.testGetValue()?.firstOrNull()
        assertNotNull(telemetry)
    }

    @Test
    fun `WHEN TranslateExceptionAction for Translate operation is dispatched THEN update telemetry`() {
        assertNull(Translations.translateFailed.testGetValue())

        // Shouldn't record other operations
        store.dispatch(
            TranslationsAction.TranslateExceptionAction(
                tabId = "1",
                operation = TranslationOperation.FETCH_SUPPORTED_LANGUAGES,
                translationError = TranslationError.UnknownError(IllegalStateException()),
            ),
        ).joinBlocking()
        assertNull(Translations.translateFailed.testGetValue())

        // Should record translate operations
        store.dispatch(
            TranslationsAction.TranslateExceptionAction(
                tabId = "1",
                operation = TranslationOperation.TRANSLATE,
                translationError = TranslationError.CouldNotTranslateError(null),
            ),
        ).joinBlocking()

        val telemetry = Translations.translateFailed.testGetValue()?.firstOrNull()
        assertEquals(TranslationError.CouldNotTranslateError(cause = null).errorName, telemetry?.extra?.get("error"))
    }

    @Test
    fun `WHEN SetEngineSupportedAction is dispatched AND supported THEN update telemetry`() {
        assertNull(Translations.engineSupported.testGetValue())

        store.dispatch(
            TranslationsAction.SetEngineSupportedAction(
                isEngineSupported = true,
            ),
        ).joinBlocking()

        val telemetry = Translations.engineSupported.testGetValue()?.firstOrNull()
        assertEquals("supported", telemetry?.extra?.get("support"))
    }

    @Test
    fun `WHEN SetEngineSupportedAction is dispatched AND unsupported THEN update telemetry`() {
        assertNull(Translations.engineSupported.testGetValue())

        store.dispatch(
            TranslationsAction.SetEngineSupportedAction(
                isEngineSupported = false,
            ),
        ).joinBlocking()

        val telemetry = Translations.engineSupported.testGetValue()?.firstOrNull()
        assertEquals("unsupported", telemetry?.extra?.get("support"))
    }
}

internal class FakeClock : Clock.Delegate {
    var elapsedTime: Long = 0
    override fun elapsedRealtime(): Long = elapsedTime
}
