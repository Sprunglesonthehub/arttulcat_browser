/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.tabs

import kotlinx.coroutines.test.runTest
import mozilla.components.browser.session.storage.RecoverableBrowserState
import mozilla.components.browser.session.storage.SessionStorage
import mozilla.components.browser.state.action.DefaultDesktopModeAction
import mozilla.components.browser.state.action.EngineAction
import mozilla.components.browser.state.action.TabListAction
import mozilla.components.browser.state.engine.EngineMiddleware
import mozilla.components.browser.state.selector.findNormalOrPrivateTabByUrl
import mozilla.components.browser.state.selector.findTab
import mozilla.components.browser.state.selector.selectedTab
import mozilla.components.browser.state.state.TabSessionState
import mozilla.components.browser.state.state.createTab
import mozilla.components.browser.state.state.recover.RecoverableTab
import mozilla.components.browser.state.state.recover.toRecoverableTab
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.engine.Engine
import mozilla.components.concept.engine.EngineSession
import mozilla.components.concept.engine.EngineSession.LoadUrlFlags
import mozilla.components.concept.engine.EngineSessionState
import mozilla.components.concept.storage.HistoryMetadataKey
import mozilla.components.support.test.any
import mozilla.components.support.test.argumentCaptor
import mozilla.components.support.test.ext.joinBlocking
import mozilla.components.support.test.libstate.ext.waitUntilIdle
import mozilla.components.support.test.mock
import mozilla.components.support.test.rule.MainCoroutineRule
import mozilla.components.support.test.whenever
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.mockito.ArgumentMatchers.anyBoolean
import org.mockito.ArgumentMatchers.anyString
import org.mockito.Mockito.never
import org.mockito.Mockito.times
import org.mockito.Mockito.verify

const val DAY_IN_MS = 24 * 60 * 60 * 1000L

class TabsUseCasesTest {

    private lateinit var store: BrowserStore
    private lateinit var tabsUseCases: TabsUseCases
    private lateinit var engine: Engine
    private lateinit var engineSession: EngineSession

    @get:Rule
    val coroutinesTestRule = MainCoroutineRule()
    private val dispatcher = coroutinesTestRule.testDispatcher

    @Before
    fun setup() {
        engineSession = mock()
        engine = mock()

        whenever(engine.createSession(anyBoolean(), any())).thenReturn(engineSession)
        store = BrowserStore(
            middleware = EngineMiddleware.create(
                engine = engine,
            ),
        )
        tabsUseCases = TabsUseCases(store)
    }

    @Test
    fun `SelectTabUseCase - tab is marked as selected in store`() {
        val tab = createTab("https://mozilla.org")
        val otherTab = createTab("https://firefox.com")
        store.dispatch(TabListAction.AddTabAction(otherTab)).joinBlocking()
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(otherTab.id, store.state.selectedTabId)
        assertEquals(otherTab, store.state.selectedTab)

        tabsUseCases.selectTab(tab.id)
        store.waitUntilIdle()
        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
    }

    @Test
    fun `RemoveTabUseCase - session will be removed from store`() {
        val tab = createTab("https://mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        assertEquals(1, store.state.tabs.size)

        tabsUseCases.removeTab(tab.id)
        store.waitUntilIdle()
        assertEquals(0, store.state.tabs.size)
    }

    @Test
    fun `RemoveTabUseCase - remove by ID and select parent if it exists`() {
        val parentTab = createTab("https://firefox.com")
        store.dispatch(TabListAction.AddTabAction(parentTab)).joinBlocking()

        val tab = createTab("https://mozilla.org", parent = parentTab)
        store.dispatch(TabListAction.AddTabAction(tab, select = true)).joinBlocking()
        assertEquals(2, store.state.tabs.size)
        assertEquals(tab.id, store.state.selectedTabId)

        tabsUseCases.removeTab(tab.id, selectParentIfExists = true)
        store.waitUntilIdle()
        assertEquals(1, store.state.tabs.size)
        assertEquals(parentTab.id, store.state.selectedTabId)
    }

    @Test
    fun `RemoveTabsUseCase - list of sessions can be removed`() {
        val tab = createTab("https://mozilla.org")
        val otherTab = createTab("https://firefox.com")
        store.dispatch(TabListAction.AddTabAction(otherTab)).joinBlocking()
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(otherTab.id, store.state.selectedTabId)
        assertEquals(otherTab, store.state.selectedTab)

        tabsUseCases.removeTabs(listOf(tab.id, otherTab.id))
        store.waitUntilIdle()
        assertEquals(0, store.state.tabs.size)
    }

    @Test
    fun `AddNewTabUseCase - session will be added to store`() {
        tabsUseCases.addTab("https://www.mozilla.org")

        store.waitUntilIdle()
        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertFalse(store.state.tabs[0].content.private)
    }

    @Test
    fun `AddNewTabUseCase - private session will be added to store`() {
        tabsUseCases.addTab("https://www.mozilla.org", private = true)

        store.waitUntilIdle()
        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertTrue(store.state.tabs[0].content.private)
    }

    @Test
    fun `AddNewTabUseCase will not load URL if flag is set to false`() {
        tabsUseCases.addTab("https://www.mozilla.org", startLoading = false)

        store.waitUntilIdle()
        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        verify(engineSession, never()).loadUrl(anyString(), any(), any(), any(), any(), anyBoolean())
    }

    @Test
    fun `AddNewTabUseCase will load URL if flag is set to true`() {
        tabsUseCases.addTab("https://www.mozilla.org", startLoading = true)

        // Wait for CreateEngineSessionAction and middleware
        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        // Wait for LinkEngineSessionAction and middleware
        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        verify(engineSession, times(1)).loadUrl("https://www.mozilla.org")
    }

    @Test
    fun `AddNewTabUseCase forwards load flags to engine`() {
        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            startLoading = true,
            textDirectiveUserActivation = true,
        )

        // Wait for CreateEngineSessionAction and middleware
        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        // Wait for LinkEngineSessionAction and middleware
        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        verify(engineSession, times(1)).loadUrl("https://www.mozilla.org", null, LoadUrlFlags.external(), null, null, true)
    }

    @Test
    fun `AddNewTabUseCase uses provided engine session`() {
        val session: EngineSession = mock()
        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            startLoading = true,
            engineSession = session,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertSame(session, store.state.tabs[0].engineState.engineSession)
    }

    @Test
    fun `AddNewTabUseCase uses provided contextId`() {
        val contextId = "1"
        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            startLoading = true,
            contextId = contextId,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertEquals(contextId, store.state.tabs[0].contextId)
    }

    @Test
    fun `AddNewTabUseCase uses provided title`() {
        val title = "Mozilla"
        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            startLoading = true,
            title = title,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertEquals(title, store.state.tabs[0].content.title)
    }

    @Test
    fun `AddNewTabUseCase uses provided history metadata`() {
        val historyMetadata = HistoryMetadataKey(
            "https://www.mozilla.org",
            searchTerm = "test",
            referrerUrl = "http://firefox.com",
        )

        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            startLoading = true,
            historyMetadata = historyMetadata,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertEquals(historyMetadata, store.state.tabs[0].historyMetadata)
    }

    @Test
    fun `GIVEN a search is performed WHEN adding a new tab THEN the resulting tab is flagged as the result of a search`() {
        tabsUseCases.addTab.invoke(
            "https://www.mozilla.org",
            flags = LoadUrlFlags.external(),
            isSearch = true,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertEquals(true, store.state.tabs.single().content.isSearch)
    }

    @Test
    fun `GIVEN a search is performed with load URL flags and additional headers WHEN adding a new tab THEN the resulting tab is loaded as a search result with the correct load flags and headers`() {
        val url = "https://www.mozilla.org"
        val flags = LoadUrlFlags.select(LoadUrlFlags.ALLOW_ADDITIONAL_HEADERS)
        val additionalHeaders = mapOf("X-Extra-Header" to "true")

        tabsUseCases.addTab.invoke(
            url = url,
            flags = flags,
            isSearch = true,
            additionalHeaders = additionalHeaders,
        )

        store.waitUntilIdle()

        assertEquals(1, store.state.tabs.size)
        assertTrue(store.state.tabs.single().content.isSearch)
        assertEquals(flags, store.state.tabs.single().engineState.initialLoadFlags)
        assertEquals(
            additionalHeaders,
            store.state.tabs.single().engineState.initialAdditionalHeaders,
        )

        verify(engineSession, times(1)).loadUrl(
            url = url,
            flags = flags,
            additionalHeaders = additionalHeaders,
        )
    }

    @Test
    fun `GIVEN a tab is added with a parent loadURL will include the parent`() {
        val parentTabId = tabsUseCases.addTab(url = "https://www.firefox.com", selectTab = true)
        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        verify(engineSession, times(1)).loadUrl(
            url = "https://www.firefox.com",
            parent = null,
            flags = LoadUrlFlags.none(),
            additionalHeaders = null,
        )

        assertEquals(1, store.state.tabs.size)

        tabsUseCases.addTab(url = "https://www.mozilla.org", parentId = parentTabId)

        store.waitUntilIdle()
        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(2, store.state.tabs.size)
        verify(engineSession, times(1)).loadUrl(
            url = "https://www.mozilla.org",
            parent = engineSession,
            flags = LoadUrlFlags.none(),
            additionalHeaders = null,
        )
    }

    @Test
    fun `RemoveAllTabsUseCase will remove all sessions`() {
        val tab = createTab("https://mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        assertEquals(1, store.state.tabs.size)

        val tab2 = createTab("https://firefox.com", private = true)
        store.dispatch(TabListAction.AddTabAction(tab2)).joinBlocking()
        assertEquals(2, store.state.tabs.size)

        tabsUseCases.removeAllTabs()
        store.waitUntilIdle()
        assertEquals(0, store.state.tabs.size)
    }

    @Test
    fun `RemoveNormalTabsUseCase and RemovePrivateTabsUseCase will remove sessions for particular type of tabs private or normal`() {
        val tab = createTab("https://mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        assertEquals(1, store.state.tabs.size)

        val privateTab = createTab("https://firefox.com", private = true)
        store.dispatch(TabListAction.AddTabAction(privateTab)).joinBlocking()
        assertEquals(2, store.state.tabs.size)

        tabsUseCases.removeNormalTabs()
        store.waitUntilIdle()
        assertEquals(1, store.state.tabs.size)

        tabsUseCases.removePrivateTabs()
        store.waitUntilIdle()
        assertEquals(0, store.state.tabs.size)
    }

    @Test
    fun `RestoreUseCase - filters based on tab timeout`() = runTest {
        val useCases = TabsUseCases(BrowserStore())

        val now = System.currentTimeMillis()
        val twoDays = now - 2 * DAY_IN_MS
        val threeDays = now - 3 * DAY_IN_MS
        val tabs = listOf(
            createTab("https://mozilla.org", lastAccess = 0).toRecoverableTab(),
            createTab("https://mozilla.org", lastAccess = now).toRecoverableTab(),
            createTab("https://firefox.com", lastAccess = twoDays, createdAt = threeDays).toRecoverableTab(),
            createTab("https://getpocket.com", lastAccess = threeDays, createdAt = threeDays).toRecoverableTab(),
        )

        val sessionStorage: SessionStorage = mock()
        useCases.restore(sessionStorage, tabTimeoutInMs = DAY_IN_MS)

        val predicateCaptor = argumentCaptor<(RecoverableTab) -> Boolean>()
        verify(sessionStorage).restore(predicateCaptor.capture())

        // Only the first two tab should be restored, all others "timed out."
        val restoredTabs = tabs.filter(predicateCaptor.value)
        assertEquals(2, restoredTabs.size)
        assertEquals(tabs.first(), restoredTabs.first())
    }

    @Test
    fun `GIVEN the previous browser session has not yet been restored WHEN the user opens a new tab THEN the restored tabs should be placed before the newly opened tab`() = runTest {
        val newTab = createTab("https://www.example.org")
        val restoredTabs = listOf(
            createTab("https://mozilla.org"),
            createTab("https://mozilla.org"),
            createTab("https://firefox.com"),
            createTab("https://getpocket.com"),
        )
        val recoverableBrowserState = RecoverableBrowserState(
            tabs = restoredTabs.map { it.toRecoverableTab() },
            selectedTabId = null,
        )
        val sessionStorage: SessionStorage = mock()
        whenever(sessionStorage.restore(any())).thenReturn(recoverableBrowserState)

        store.dispatch(TabListAction.AddTabAction(tab = newTab)).joinBlocking()

        tabsUseCases.restore.invoke(
            storage = sessionStorage,
            tabTimeoutInMs = DAY_IN_MS,
        )

        store.waitUntilIdle()

        assertEquals(restoredTabs.first().id, store.state.tabs.first().id)
        assertEquals(newTab.id, store.state.tabs.last().id)
    }

    @Test
    fun `selectOrAddTab selects already existing tab`() {
        val tab = createTab("https://mozilla.org")
        val otherTab = createTab("https://firefox.com")

        store.dispatch(TabListAction.AddTabAction(otherTab)).joinBlocking()
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(otherTab.id, store.state.selectedTabId)
        assertEquals(otherTab, store.state.selectedTab)
        assertEquals(2, store.state.tabs.size)

        val tabID = tabsUseCases.selectOrAddTab(tab.content.url)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(tab.id, tabID)
    }

    @Test
    fun `selectOrAddTab selects already existing tab with matching historyMetadata`() {
        val historyMetadata = HistoryMetadataKey(
            url = "https://mozilla.org",
            referrerUrl = "https://firefox.com",
        )

        val tab = createTab("https://mozilla.org", historyMetadata = historyMetadata)
        val otherTab = createTab("https://firefox.com")

        store.dispatch(TabListAction.AddTabAction(otherTab)).joinBlocking()
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(otherTab.id, store.state.selectedTabId)
        assertEquals(otherTab, store.state.selectedTab)
        assertEquals(2, store.state.tabs.size)

        val tabID = tabsUseCases.selectOrAddTab(tab.content.url, historyMetadata = historyMetadata)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(tab.id, tabID)
    }

    @Test
    fun `selectOrAddTab adds new tab if no matching existing tab could be found`() {
        val tab = createTab("https://mozilla.org")

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(1, store.state.tabs.size)

        val tabID = tabsUseCases.selectOrAddTab("https://firefox.com")
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertNotNull(store.state.findNormalOrPrivateTabByUrl("https://firefox.com", false))
        assertEquals(store.state.selectedTabId, tabID)
    }

    @Test
    fun `selectOrAddTab adds new tab if no matching existing history metadata could be found`() {
        val tab = createTab("https://mozilla.org")
        val historyMetadata = HistoryMetadataKey(
            url = "https://mozilla.org",
            referrerUrl = "https://firefox.com",
        )

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(1, store.state.tabs.size)

        val tabID =
            tabsUseCases.selectOrAddTab("https://firefox.com", historyMetadata = historyMetadata)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertNotNull(store.state.findNormalOrPrivateTabByUrl("https://firefox.com", false))
        assertEquals(store.state.selectedTabId, tabID)
    }

    @Test
    fun `selectOrAddTab selects already existing tab with matching url when ignoreFragment is set to true`() {
        val tab = createTab("https://mozilla.org")
        val otherTab = createTab("https://firefox.com")

        store.dispatch(TabListAction.AddTabAction(otherTab)).joinBlocking()
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(otherTab, store.state.selectedTab)
        assertEquals(2, store.state.tabs.size)

        val actualTabId = tabsUseCases.selectOrAddTab(url = "https://mozilla.org/#welcome", ignoreFragment = true)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(store.state.selectedTabId, actualTabId)
    }

    @Test
    fun `selectOrAddTab adds new tab if no matching existing tab could be found with ignoreFragment set to true`() {
        val tab = createTab("https://mozilla.org")

        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        assertEquals(tab.id, store.state.selectedTabId)
        assertEquals(tab, store.state.selectedTab)
        assertEquals(1, store.state.tabs.size)

        val tabID = tabsUseCases.selectOrAddTab(url = "https://firefox.com", ignoreFragment = true)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals(store.state.selectedTabId, tabID)
    }

    @Test
    fun `selectOrAddTab adds new tab when store desktop mode is false and tab's desktop mode matches`() {
        assertEquals(false, store.state.desktopMode)

        val tabID = tabsUseCases.selectOrAddTab(url = "https://firefox.com")
        store.waitUntilIdle()

        assertEquals(store.state.selectedTabId, tabID)
        assertEquals(false, store.state.selectedTab?.content?.desktopMode)
    }

    @Test
    fun `selectOrAddTab adds new tab when store desktop mode is true and tab's desktop mode matches`() {
        store.dispatch(DefaultDesktopModeAction.ToggleDesktopMode).joinBlocking()
        assertEquals(true, store.state.desktopMode)

        val tabID = tabsUseCases.selectOrAddTab(url = "https://firefox.com")
        store.waitUntilIdle()

        assertEquals(store.state.selectedTabId, tabID)
        assertEquals(true, store.state.selectedTab?.content?.desktopMode)
    }

    private fun assertTabsDuplicates(tab: TabSessionState, dup: TabSessionState) {
        assertEquals(tab.content.url, dup.content.url)
        assertEquals(tab.content.private, dup.content.private)
        assertEquals(tab.contextId, dup.contextId)
        assertEquals(tab.engineState.engineSessionState, dup.engineState.engineSessionState)
        assertNotEquals(tab.id, dup.id)
        assertEquals(tab.id, dup.parentId)
    }

    @Test
    fun `duplicateTab creates a duplicate of the given tab`() {
        store.dispatch(
            TabListAction.AddTabAction(
                createTab(id = "mozilla", url = "https://www.mozilla.org"),
            ),
        ).joinBlocking()
        assertEquals(1, store.state.tabs.size)

        val engineSessionState: EngineSessionState = mock()
        store.dispatch(
            EngineAction.UpdateEngineSessionStateAction("mozilla", engineSessionState),
        ).joinBlocking()

        val tab = store.state.findTab("mozilla")!!
        val dupId = tabsUseCases.duplicateTab.invoke(tab)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals(dupId, store.state.tabs[1].id)
        assertEquals("mozilla", store.state.tabs[0].id)
        assertFalse(store.state.tabs[0].content.private)
        assertEquals("https://www.mozilla.org", store.state.tabs[0].content.url)
        assertEquals(engineSessionState, store.state.tabs[0].engineState.engineSessionState)
        assertNull(store.state.tabs[0].parentId)
        assertTabsDuplicates(tab, store.state.tabs[1])
    }

    @Test
    fun `duplicateTab creates duplicates of private tabs`() {
        val tab = createTab(id = "mozilla", url = "https://www.mozilla.org", private = true)
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        tabsUseCases.duplicateTab.invoke(tab)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertTrue(store.state.tabs[0].content.private)
        assertTrue(store.state.tabs[1].content.private)
    }

    @Test
    fun `duplicateTab keeps contextId`() {
        val tab = createTab(id = "mozilla", url = "https://www.mozilla.org", contextId = "work")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        tabsUseCases.duplicateTab.invoke(tab)
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertEquals("work", store.state.tabs[0].contextId)
        assertEquals("work", store.state.tabs[1].contextId)
    }

    @Test
    fun `duplicateTab without tab argument uses the selected tab`() {
        var tab = createTab(url = "https://www.mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        var dupId = tabsUseCases.duplicateTab.invoke(selectNewTab = true)!!
        store.waitUntilIdle()

        assertEquals(2, store.state.tabs.size)
        assertNotNull(dupId)
        var dup = store.state.findTab(dupId)!!
        assertTabsDuplicates(tab, dup)
        assertEquals(dup, store.state.selectedTab)

        tab = dup
        dupId = tabsUseCases.duplicateTab.invoke(selectNewTab = false)!!
        store.waitUntilIdle()

        assertEquals(3, store.state.tabs.size)
        assertNotNull(dupId)
        dup = store.state.findTab(dupId)!!
        assertTabsDuplicates(tab, dup)
        assertEquals(tab, store.state.selectedTab)
    }

    @Test
    fun `MoveTabsUseCase will move a tab`() {
        val tab = createTab("https://mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        val tab2 = createTab("https://firefox.com", private = true)
        store.dispatch(TabListAction.AddTabAction(tab2)).joinBlocking()
        assertEquals(2, store.state.tabs.size)
        assertEquals("https://mozilla.org", store.state.tabs[0].content.url)
        assertEquals("https://firefox.com", store.state.tabs[1].content.url)

        val tab1Id = store.state.tabs[0].id
        val tab2Id = store.state.tabs[1].id
        tabsUseCases.moveTabs(listOf(tab1Id), tab2Id, true)
        store.waitUntilIdle()
        assertEquals("https://firefox.com", store.state.tabs[0].content.url)
        assertEquals("https://mozilla.org", store.state.tabs[1].content.url)
    }

    @Test
    fun `MigratePrivateTabUseCase will migrate a private tab`() {
        val tab = createTab("https://mozilla.org", private = true)
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        assertEquals(1, store.state.tabs.size)
        assertEquals(true, store.state.tabs[0].content.private)

        tabsUseCases.migratePrivateTabUseCase(tab.id)
        store.waitUntilIdle()
        // Still only 1 tab and that tab still has the same URL...
        assertEquals(1, store.state.tabs.size)
        assertEquals("https://mozilla.org", store.state.tabs[0].content.url)
        // But it's no longer private and has a different tabId.
        assertEquals(false, store.state.tabs[0].content.private)
        assertNotEquals(store.state.tabs[0].id, tab.id)
    }

    @Test
    fun `MigratePrivateTabUseCase will respect alternativeUrl`() {
        // This (obviously!) isn't a real reader-mode URL, but is fine for the purposes of this test.
        val tab = createTab("https://mozilla.org/reader-mode", private = true)
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()

        tabsUseCases.migratePrivateTabUseCase(store.state.tabs[0].id, "https://mozilla.org/not-reader-mode")
        store.waitUntilIdle()
        // Still only 1 tab with our alternative URL
        assertEquals(1, store.state.tabs.size)
        assertEquals("https://mozilla.org/not-reader-mode", store.state.tabs[0].content.url)
        assertEquals(false, store.state.tabs[0].content.private)
    }

    @Test
    fun `MigratePrivateTabUseCase will fail on a regular tab`() {
        val tab = createTab("https://mozilla.org")
        store.dispatch(TabListAction.AddTabAction(tab)).joinBlocking()
        assertEquals(1, store.state.tabs.size)
        assertThrows(IllegalArgumentException::class.java) {
            tabsUseCases.migratePrivateTabUseCase(tab.id)
        }
    }

    @Test
    fun `MigratePrivateTabUseCase will fail if the tab can't be found`() {
        assertThrows(IllegalStateException::class.java) {
            tabsUseCases.migratePrivateTabUseCase("invalid-tab-id")
        }
    }
}
