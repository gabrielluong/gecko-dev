/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.home.topsites.utils

import androidx.core.net.toUri
import androidx.test.ext.junit.runners.AndroidJUnit4
import mozilla.components.feature.top.sites.TopSite
import mozilla.components.support.ktx.kotlin.sha1
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.random.Random.Default.nextLong

@RunWith(AndroidJUnit4::class)
class TopSitesUtilsTest {

    @Test
    fun `GIVEN the search engine name is eBay WHEN should show sponsored top site is called THEN return false`() {
        val eBayTopSite = createSponsoredTopSite(title = EBAY_SPONSORED_TITLE, url = "https://www.eay.com")

        assertFalse(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = eBayTopSite,
                searchEngineName = EBAY_SPONSORED_TITLE,
                blocklist = setOf(),
            ),
        )
    }

    @Test
    fun `GIVEN the search engine name is Amazon WHEN should show sponsored top site is called THEN return false`() {
        val amazonTopSite = createSponsoredTopSite(title = AMAZON_SPONSORED_TITLE, "https://www.amazon.com")

        assertFalse(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = amazonTopSite,
                searchEngineName = AMAZON_SEARCH_ENGINE_NAME,
                blocklist = setOf(),
            ),
        )
    }

    @Test
    fun `GIVEN no search engine name and no blocklist WHEN should show sponsored top site is called THEN return true`() {
        val firefoxTopSite = createSponsoredTopSite(title = "Firefox", "https://www.mozilla.org")

        assertTrue(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = firefoxTopSite,
                searchEngineName = "",
                blocklist = setOf(),
            ),
        )
    }

    @Test
    fun `GIVEN a sponsored top site blocklist WHEN should show sponsored top site is called THEN return the appropriate result`() {
        val eBayTopSite = createSponsoredTopSite(title = EBAY_SPONSORED_TITLE, url = "https://www.eay.com")
        val amazonTopSite = createSponsoredTopSite(title = AMAZON_SPONSORED_TITLE, "https://www.amazon.com")
        val firefoxTopSite = createSponsoredTopSite(title = "Firefox", "https://www.mozilla.org")

        val blocklist = setOfNotNull(
            eBayTopSite.url.toUri().host?.sha1(),
            amazonTopSite.url.toUri().host?.sha1(),
        )

        assertFalse(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = eBayTopSite,
                searchEngineName = "",
                blocklist = blocklist,
            ),
        )
        assertFalse(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = amazonTopSite,
                searchEngineName = "",
                blocklist = blocklist,
            ),
        )
        assertTrue(
            TopSitesUtils.shouldShowSponsoredTopSite(
                topSite = firefoxTopSite,
                searchEngineName = "",
                blocklist = blocklist,
            ),
        )
    }

    private fun createSponsoredTopSite(
        title: String,
        url: String,
    ) = TopSite.Provided(
        id = nextLong(),
        title = title,
        url = url,
        clickUrl = "",
        imageUrl = "",
        impressionUrl = "",
        createdAt = System.currentTimeMillis(),
    )
}
