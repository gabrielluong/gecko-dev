/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.tabstrip

import androidx.navigation.NavController
import mozilla.components.feature.tabs.TabsUseCases
import org.mozilla.fenix.browser.BrowserFragmentDirections
import org.mozilla.fenix.browser.browsingmode.BrowsingModeManager
import org.mozilla.fenix.home.sessioncontrol.TabStripInteractor

/**
 * An interface that handles the view manipulation of the tab strip in the Home screen.
 */
interface TabStripController {

    /**
     * @see [TabStripInteractor.onTabStripAddTabClicked]
     */
    fun handleTabStripAddTabClick()
}

/**
 * The default implementation of [TabStripController].
 */
class DefaultTabStripController(
    private val browsingModeManager: BrowsingModeManager,
    private val navController: NavController,
    private val addTabUseCase: TabsUseCases.AddNewTabUseCase,
) : TabStripController {

    override fun handleTabStripAddTabClick() {
        addTabUseCase.invoke(
            startLoading = false,
            private = browsingModeManager.mode.isPrivate,
        )

        navController.navigate(
            BrowserFragmentDirections.actionGlobalHome(focusOnAddressBar = true),
        )
    }
}
