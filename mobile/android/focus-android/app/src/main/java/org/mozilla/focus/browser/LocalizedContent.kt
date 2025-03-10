/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
package org.mozilla.focus.browser

import android.content.Context
import android.view.View
import androidx.collection.ArrayMap
import org.mozilla.focus.R
import org.mozilla.focus.locale.Locales
import org.mozilla.focus.utils.HtmlLoader

object LocalizedContent {
    // We can't use "about:" because webview silently swallows about: pages, hence we use
    // a custom scheme.
    const val URL_RIGHTS = "focus:rights"
    const val URL_GPL = "focus:gpl"
    const val URL_LICENSES = "focus:licenses"

    /**
     * Load the content for focus:rights
     */
    fun loadRights(context: Context): String {
        val resources = Locales.getLocalizedResources(context)
        val substitutionMap: MutableMap<String, String> = ArrayMap()
        val appName = context.resources.getString(R.string.app_name)
        val mplUrl = "https://www.mozilla.org/en-US/MPL/"
        val trademarkPolicyUrl = "https://www.mozilla.org/foundation/trademarks/policy/"
        val gplUrl = "focus:gpl"
        val trackingProtectionUrl = "https://wiki.mozilla.org/Security/Tracking_protection#Lists"
        val licensesUrl = "focus:licenses"
        val content1 = resources.getString(R.string.your_rights_content1, appName)
        substitutionMap["%your-rights-content1%"] = content1
        val content2 = resources.getString(R.string.your_rights_content2, appName, mplUrl)
        substitutionMap["%your-rights-content2%"] = content2
        val content3 = resources.getString(R.string.your_rights_content3, appName, trademarkPolicyUrl)
        substitutionMap["%your-rights-content3%"] = content3
        val content4 = resources.getString(R.string.your_rights_content4, appName, licensesUrl)
        substitutionMap["%your-rights-content4%"] = content4
        val content5 = resources.getString(R.string.your_rights_content5, appName, gplUrl, trackingProtectionUrl)
        substitutionMap["%your-rights-content5%"] = content5
        putLayoutDirectionIntoMap(substitutionMap, context)
        return HtmlLoader.loadResourceFile(context, R.raw.rights, substitutionMap)
    }

    fun loadLicenses(context: Context): String {
        return HtmlLoader.loadResourceFile(context, R.raw.licenses, emptyMap())
    }

    fun loadGPL(context: Context): String {
        return HtmlLoader.loadResourceFile(context, R.raw.gpl, emptyMap())
    }

    private fun putLayoutDirectionIntoMap(substitutionMap: MutableMap<String, String>, context: Context) {
        val direction: String = when (context.resources.configuration.layoutDirection) {
            View.LAYOUT_DIRECTION_LTR -> {
                "ltr"
            }
            View.LAYOUT_DIRECTION_RTL -> {
                "rtl"
            }
            else -> {
                "auto"
            }
        }
        substitutionMap["%dir%"] = direction
    }
}
