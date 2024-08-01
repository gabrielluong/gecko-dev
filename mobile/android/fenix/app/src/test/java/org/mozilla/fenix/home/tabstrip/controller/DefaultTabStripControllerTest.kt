/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.tabstrip.controller

import androidx.navigation.NavController
import io.mockk.mockk
import io.mockk.verify
import mozilla.components.feature.tabs.TabsUseCases
import org.junit.Before
import org.junit.Test
import org.mozilla.fenix.browser.BrowserFragmentDirections
import org.mozilla.fenix.browser.browsingmode.BrowsingModeManager
import org.mozilla.fenix.home.tabstrip.DefaultTabStripController

class DefaultTabStripControllerTest {

    private lateinit var browsingModeManager: BrowsingModeManager
    private lateinit var navController: NavController
    private lateinit var addTabUseCase: TabsUseCases.AddNewTabUseCase

    @Before
    fun setup() {
        browsingModeManager = mockk(relaxed = true)
        navController = mockk(relaxed = true)
        addTabUseCase = mockk(relaxed = true)
    }

    @Test
    fun `WHEN the tab strip new tab button is clicked THEN a new homepage tab is displayed`() {
        val controller = createController()
        controller.handleTabStripAddTabClick()

        verify {
            addTabUseCase.invoke(
                startLoading = false,
                private = false,
            )

            navController.navigate(
                BrowserFragmentDirections.actionGlobalHome(focusOnAddressBar = true),
            )
        }
    }

    private fun createController() = DefaultTabStripController(
        browsingModeManager = browsingModeManager,
        navController = navController,
        addTabUseCase = addTabUseCase,
    )
}
