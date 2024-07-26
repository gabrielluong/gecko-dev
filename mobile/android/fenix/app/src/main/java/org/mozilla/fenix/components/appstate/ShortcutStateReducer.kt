/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.appstate

import mozilla.components.feature.top.sites.TopSite
import org.mozilla.fenix.components.appstate.AppAction.ShortcutAction
import org.mozilla.fenix.components.appstate.snackbar.SnackbarState

/**
 * A [ShortcutAction] reducer that updates [AppState].
 */
internal object ShortcutStateReducer {
    fun reduce(state: AppState, action: ShortcutAction): AppState = when (action) {
        is ShortcutAction.ShortcutAdded -> state.copy(
            snackbarState = SnackbarState.ShortcutAdded,
        )

        is ShortcutAction.ShortcutRemoved -> state.copy(
            snackbarState = SnackbarState.ShortcutRemoved,
        )

        is ShortcutAction.SponsoredShortcutRemoved -> state.copy(
            topSites = state.topSites.filterNot { topSite ->
                topSite is TopSite.Provided && topSite.url == action.topSite.url
            },
        )
    }
}
