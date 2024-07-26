/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.appstate

import mozilla.components.feature.top.sites.TopSite
import mozilla.components.support.test.ext.joinBlocking
import org.junit.Assert.assertEquals
import org.junit.Test
import org.mozilla.fenix.components.AppStore
import org.mozilla.fenix.components.appstate.snackbar.SnackbarState
import org.mozilla.fenix.settings.SupportUtils

class ShortcutStateReducerTest {

    private val defaultGoogleTopSite = TopSite.Default(
        id = 1L,
        title = "Google",
        url = SupportUtils.GOOGLE_URL,
        createdAt = 0,
    )
    private val providedSite1 = TopSite.Provided(
        id = 3,
        title = "Mozilla",
        url = "https://mozilla.com",
        clickUrl = "https://mozilla.com/click",
        imageUrl = "https://test.com/image2.jpg",
        impressionUrl = "https://example.com",
        createdAt = 3,
    )
    private val providedSite2 = TopSite.Provided(
        id = 3,
        title = "Firefox",
        url = "https://firefox.com",
        clickUrl = "https://firefox.com/click",
        imageUrl = "https://test.com/image2.jpg",
        impressionUrl = "https://example.com",
        createdAt = 3,
    )
    private val pinnedSite1 = TopSite.Pinned(
        id = 1L,
        title = "DuckDuckGo",
        url = "https://duckduckgo.com",
        createdAt = 0,
    )
    private val pinnedSite2 = TopSite.Pinned(
        id = 1L,
        title = "Mozilla",
        url = "mozilla.org",
        createdAt = 0,
    )
    private val frecentSite = TopSite.Frecent(
        id = 1L,
        title = "Mozilla",
        url = "mozilla.org",
        createdAt = 0,
    )

    @Test
    fun `WHEN shortcut added action is dispatched THEN state is updated`() {
        val appStore = AppStore()

        appStore.dispatch(AppAction.ShortcutAction.ShortcutAdded).joinBlocking()

        assertEquals(SnackbarState.ShortcutAdded, appStore.state.snackbarState)
    }

    @Test
    fun `WHEN shortcut removed action is dispatched THEN state is updated`() {
        val appStore = AppStore()

        appStore.dispatch(AppAction.ShortcutAction.ShortcutRemoved).joinBlocking()

        assertEquals(SnackbarState.ShortcutRemoved, appStore.state.snackbarState)
    }

    @Test
    fun `WHEN shortcut dismissed action is dispatched THEN state is updated`() {
        val topSites = listOf(
            defaultGoogleTopSite,
            providedSite1,
            providedSite2,
            pinnedSite1,
            pinnedSite2,
            frecentSite,
        )
        val appStore = AppStore(
            initialState = AppState(
                topSites = topSites,
            ),
        )

        appStore.dispatch(AppAction.ShortcutAction.SponsoredShortcutRemoved(topSite = providedSite1))
            .joinBlocking()

        assertEquals(topSites.minus(providedSite1), appStore.state.topSites)
    }
}
