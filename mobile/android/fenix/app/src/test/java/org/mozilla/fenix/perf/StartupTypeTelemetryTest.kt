/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.perf

import android.app.Application
import androidx.lifecycle.Lifecycle
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.mockk.MockKAnnotations
import io.mockk.every
import io.mockk.impl.annotations.MockK
import io.mockk.mockk
import io.mockk.spyk
import io.mockk.verify
import kotlinx.coroutines.test.advanceUntilIdle
import mozilla.components.support.ktx.kotlin.crossProduct
import mozilla.components.support.test.robolectric.testContext
import mozilla.components.support.test.rule.MainCoroutineRule
import mozilla.components.support.test.rule.runTestOnMain
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.GleanMetrics.PerfStartup
import org.mozilla.fenix.helpers.FenixGleanTestRule
import org.mozilla.fenix.perf.StartupPathProvider.StartupPath

private val validTelemetryLabels = run {
    val allStates = listOf("cold", "warm", "hot", "unknown")
    val allPaths = listOf("main", "view", "unknown")

    allStates.crossProduct(allPaths) { state, path -> "${state}_$path" }.toSet()
}

@RunWith(AndroidJUnit4::class)
class StartupTypeTelemetryTest {

    @get:Rule
    val coroutinesTestRule = MainCoroutineRule()

    @get:Rule
    val gleanTestRule = FenixGleanTestRule(testContext)

    private lateinit var telemetry: StartupTypeTelemetry
    private lateinit var callbacks: StartupTypeTelemetry.StartupTypeLifecycleObserver

    @MockK
    private lateinit var pathProvider: StartupPathProvider

    private val startupStateDetector = FakeStartupStateDetector(expectedStartupState = StartupState.UNKNOWN)

    @Before
    fun setUp() {
        MockKAnnotations.init(this)
        telemetry = spyk(
            StartupTypeTelemetry(
                startupStateDetector = startupStateDetector,
                startupPathProvider = pathProvider,
            ),
        )
        callbacks = telemetry.getTestCallbacks()
    }

    @Test
    fun `WHEN attach is called THEN it is registered to the lifecycle`() {
        val lifecycle = mockk<Lifecycle>(relaxed = true)
        telemetry.attachOnHomeActivityOnCreate(lifecycle)

        verify { lifecycle.addObserver(any()) }
    }

    @Test
    fun `GIVEN all possible path and state combinations WHEN record telemetry THEN the labels are incremented the appropriate number of times`() = runTestOnMain {
        val allPossibleInputArgs = StartupState.entries.crossProduct(
            StartupPath.entries,
        ) { state, path ->
            Pair(state, path)
        }

        allPossibleInputArgs.forEach { (state, path) ->
            startupStateDetector.expectedStartupState = state
            every { pathProvider.startupPathForActivity } returns path

            telemetry.record(coroutinesTestRule.testDispatcher)
            advanceUntilIdle()
        }

        validTelemetryLabels.forEach { label ->
            // Path == NOT_SET gets bucketed with Path == UNKNOWN so we'll increment twice for those.
            val expected = if (label.endsWith("unknown")) 2 else 1
            assertEquals("label: $label", expected, PerfStartup.startupType[label].testGetValue())
        }

        // All invalid labels go to a single bucket: let's verify it has no value.
        assertNull(PerfStartup.startupType["__other__"].testGetValue())
    }

    @Test
    fun `WHEN record is called THEN telemetry is recorded with the appropriate label`() = runTestOnMain {
        startupStateDetector.expectedStartupState = StartupState.COLD
        every { pathProvider.startupPathForActivity } returns StartupPath.MAIN

        telemetry.record(coroutinesTestRule.testDispatcher)
        advanceUntilIdle()

        assertEquals(1, PerfStartup.startupType["cold_main"].testGetValue())
    }

    @Test
    fun `GIVEN the activity is launched WHEN onResume is called THEN we record the telemetry`() {
        launchApp()
        verify(exactly = 1) { telemetry.record(any()) }
    }

    @Test
    fun `GIVEN the activity is launched WHEN the activity is paused and resumed THEN record is not called`() {
        // This part of the test duplicates another test but it's needed to initialize the state of this test.
        launchApp()
        verify(exactly = 1) { telemetry.record(any()) }

        callbacks.onPause(mockk())
        callbacks.onResume(mockk())

        verify(exactly = 1) { telemetry.record(any()) } // i.e. this shouldn't be called again.
    }

    @Test
    fun `GIVEN the activity is launched WHEN the activity is stopped and resumed THEN record is called again`() {
        // This part of the test duplicates another test but it's needed to initialize the state of this test.
        launchApp()
        verify(exactly = 1) { telemetry.record(any()) }

        callbacks.onPause(mockk())
        callbacks.onStop(mockk())
        callbacks.onStart(mockk())
        callbacks.onResume(mockk())

        verify(exactly = 2) { telemetry.record(any()) } // i.e. this should be called again.
    }

    private fun launchApp() {
        // What these return isn't important.
        startupStateDetector.expectedStartupState = StartupState.COLD
        every { pathProvider.startupPathForActivity } returns StartupPath.MAIN

        callbacks.onCreate(mockk())
        callbacks.onStart(mockk())
        callbacks.onResume(mockk())
    }

    private class FakeStartupStateDetector(
        var expectedStartupState: StartupState,
    ) : StartupStateDetector {

        override fun registerInAppOnCreate(application: Application) = Unit

        override fun getStartupState(): StartupState = expectedStartupState
    }
}
