/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.topsites.utils

import androidx.core.net.toUri
import mozilla.components.feature.top.sites.TopSite
import mozilla.components.support.ktx.kotlin.sha1

// Sponsored top sites titles and search engine names used for filtering
const val AMAZON_SEARCH_ENGINE_NAME = "Amazon.com"
const val AMAZON_SPONSORED_TITLE = "Amazon"
const val EBAY_SPONSORED_TITLE = "eBay"

/**
 * A singleton providing utility functions for handling top sites.
 */
object TopSitesUtils {

    /**
     * Returns true if the sponsored top site should be shown and false otherwise.
     *
     * @param topSite The sponsored [TopSite] to be shown.
     * @param searchEngineName The current selected search engine name.
     * @param blocklist A [Set] of hashed hostname that should be blocked from being shown.
     */
    fun shouldShowSponsoredTopSite(
        topSite: TopSite,
        searchEngineName: String?,
        blocklist: Set<String>,
    ): Boolean =
        when (searchEngineName) {
            AMAZON_SEARCH_ENGINE_NAME -> topSite.title != AMAZON_SPONSORED_TITLE
            EBAY_SPONSORED_TITLE -> topSite.title != EBAY_SPONSORED_TITLE
            else -> !blocklistContainsHost(
                blocklist = blocklist,
                url = topSite.url,
            )
        }

    private fun blocklistContainsHost(blocklist: Set<String>, url: String): Boolean =
        blocklist.any { it == url.toUri().host?.sha1() }
}
