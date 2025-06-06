/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.pocket.view

import android.view.View
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.res.dimensionResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.recyclerview.widget.RecyclerView
import mozilla.components.lib.state.ext.observeAsComposableState
import mozilla.components.service.pocket.PocketStory.PocketRecommendedStory
import org.mozilla.fenix.R
import org.mozilla.fenix.components.components
import org.mozilla.fenix.compose.ComposeViewHolder
import org.mozilla.fenix.compose.home.HomeSectionHeader
import org.mozilla.fenix.home.fake.FakeHomepagePreview
import org.mozilla.fenix.home.pocket.interactor.PocketStoriesInteractor
import org.mozilla.fenix.home.pocket.ui.PocketStories
import org.mozilla.fenix.theme.FirefoxTheme
import org.mozilla.fenix.wallpapers.WallpaperState

/**
 * [RecyclerView.ViewHolder] for displaying the list of [PocketRecommendedStory]s from [AppStore].
 *
 * @param composeView [ComposeView] which will be populated with Jetpack Compose UI content.
 * @param viewLifecycleOwner [LifecycleOwner] to which this Composable will be tied to.
 * @param interactor [PocketStoriesInteractor] callback for user interaction.
 */
class PocketStoriesViewHolder(
    composeView: ComposeView,
    viewLifecycleOwner: LifecycleOwner,
    private val interactor: PocketStoriesInteractor,
) : ComposeViewHolder(composeView, viewLifecycleOwner) {

    companion object {
        val LAYOUT_ID = View.generateViewId()
    }

    @Composable
    override fun Content() {
        val horizontalPadding = dimensionResource(R.dimen.home_item_horizontal_margin)

        val homeScreenReady = components.appStore
            .observeAsComposableState { state -> state.firstFrameDrawn }.value ?: false

        val stories = components.appStore
            .observeAsComposableState { state -> state.recommendationState.pocketStories }.value

        val wallpaperState = components.appStore
            .observeAsComposableState { state -> state.wallpaperState }.value ?: WallpaperState.default

        /* This was originally done to address this perf issue:
         * https://github.com/mozilla-mobile/fenix/issues/25545 for details.
         * It was determined that Pocket content was becoming available before the first frame was
         * rendered more regularly. Including Pocket in the first render pass significantly
         * increases time-to-render in lower-end devices. By waiting until the first frame has
         * rendered, the perceived performance should increase since the app becomes active more
         * quickly. This was intended as a workaround until the Compose upgrade was completed and a
         * more robust solution could be investigated.
         */
        if (!homeScreenReady) return
        LaunchedEffect(stories) {
            // We should report back when a certain story is actually being displayed.
            // Cannot do it reliably so for now we'll just mass report everything as being displayed.
            stories?.let {
                interactor.onStoriesShown(storiesShown = it)
            }
        }

        Column(modifier = Modifier.padding(top = 72.dp)) {
            // Simple wrapper to add horizontal padding to just the header while the stories have none.
            Box(modifier = Modifier.padding(horizontal = horizontalPadding)) {
                HomeSectionHeader(
                    headerText = stringResource(R.string.pocket_stories_header_2),
                )
            }

            Spacer(Modifier.height(16.dp))

            PocketStories(
                stories = stories ?: emptyList(),
                contentPadding = horizontalPadding,
                backgroundColor = wallpaperState.cardBackgroundColor,
                onStoryShown = interactor::onStoryShown,
                onStoryClicked = interactor::onStoryClicked,
            )
        }
    }
}

@Composable
@Preview
fun PocketStoriesViewHolderPreview() {
    FirefoxTheme {
        Column {
            HomeSectionHeader(
                headerText = stringResource(R.string.pocket_stories_header_2),
            )

            Spacer(Modifier.height(16.dp))

            @Suppress("MagicNumber")
            PocketStories(
                stories = FakeHomepagePreview.pocketStories(limit = 8),
                contentPadding = 0.dp,
                backgroundColor = FirefoxTheme.colors.layer2,
                onStoryShown = { _, _ -> },
                onStoryClicked = { _, _ -> },
            )
        }
    }
}
