/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.settings.about

import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.content.pm.PackageInfoCompat
import androidx.fragment.app.Fragment
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.DividerItemDecoration
import mozilla.components.support.utils.ext.getPackageInfoCompat
import org.mozilla.fenix.BrowserDirection
import org.mozilla.fenix.BuildConfig
import org.mozilla.fenix.GleanMetrics.Events
import org.mozilla.fenix.HomeActivity
import org.mozilla.fenix.R
import org.mozilla.fenix.crashes.CrashListActivity
import org.mozilla.fenix.databinding.FragmentAboutBinding
import org.mozilla.fenix.ext.settings
import org.mozilla.fenix.ext.showToolbar
import org.mozilla.fenix.settings.SupportUtils
import org.mozilla.fenix.settings.about.AboutItemType.LICENSING_INFO
import org.mozilla.fenix.settings.about.AboutItemType.PRIVACY_NOTICE
import org.mozilla.fenix.settings.about.AboutItemType.RIGHTS
import org.mozilla.fenix.settings.about.AboutItemType.SUPPORT
import org.mozilla.fenix.settings.about.AboutItemType.WHATS_NEW
import org.mozilla.fenix.whatsnew.WhatsNew
import org.mozilla.geckoview.BuildConfig as GeckoViewBuildConfig

/**
 * Displays the logo and information about the app, including library versions.
 */
class AboutFragment : Fragment(), AboutPageListener {

    private lateinit var appName: String
    private var aboutPageAdapter: AboutPageAdapter? = AboutPageAdapter(this)
    private var _binding: FragmentAboutBinding? = null

    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        _binding = FragmentAboutBinding.inflate(inflater, container, false)
        appName = getString(R.string.app_name)

        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        if (aboutPageAdapter == null) {
            aboutPageAdapter = AboutPageAdapter(this)
        }

        binding.aboutList.run {
            adapter = aboutPageAdapter
            addItemDecoration(
                DividerItemDecoration(
                    context,
                    DividerItemDecoration.VERTICAL,
                ),
            )
        }

        lifecycle.addObserver(
            SecretDebugMenuTrigger(
                logoView = binding.wordmark,
                settings = view.context.settings(),
            ),
        )

        populateAboutHeader()
        aboutPageAdapter?.submitList(populateAboutList())
    }

    override fun onResume() {
        super.onResume()
        showToolbar(getString(R.string.preferences_about, appName))
    }

    override fun onDestroyView() {
        super.onDestroyView()
        aboutPageAdapter = null
        _binding = null
    }

    private fun populateAboutHeader() {
        val aboutText = try {
            val packageInfo =
                requireContext().packageManager.getPackageInfoCompat(requireContext().packageName, 0)
            val versionCode = PackageInfoCompat.getLongVersionCode(packageInfo).toString()
            val maybeFenixVcsHash = if (BuildConfig.VCS_HASH.isNotBlank()) ", ${BuildConfig.VCS_HASH}" else ""
            val maybeGecko = getString(R.string.gecko_view_abbreviation)
            val geckoVersion =
                GeckoViewBuildConfig.MOZ_APP_VERSION + "-" + GeckoViewBuildConfig.MOZ_APP_BUILDID
            val appServicesAbbreviation = getString(R.string.app_services_abbreviation)
            val appServicesVersion = mozilla.components.Build.applicationServicesVersion
            val operatingSystemAbbrevation = "OS"
            val operatingSystemVersion = "Android ${Build.VERSION.RELEASE}"

            String.format(
                "%s (Build #%s)%s\n%s: %s\n%s: %s\n%s: %s",
                packageInfo.versionName,
                versionCode,
                maybeFenixVcsHash,
                maybeGecko,
                geckoVersion,
                appServicesAbbreviation,
                appServicesVersion,
                operatingSystemAbbrevation,
                operatingSystemVersion,
            )
        } catch (e: PackageManager.NameNotFoundException) {
            ""
        }

        val content = getString(R.string.about_content, appName)
        val buildDate = BuildConfig.BUILD_DATE

        binding.aboutText.text = aboutText
        binding.aboutContent.text = content
        binding.buildDate.text = buildDate
    }

    private fun populateAboutList(): List<AboutPageItem> {
        val context = requireContext()

        return listOf(
            AboutPageItem(
                AboutItem.ExternalLink(
                    WHATS_NEW,
                    SupportUtils.WHATS_NEW_URL,
                ),
                // Note: Fenix only has release notes for 'Release' versions, NOT 'Beta' & 'Nightly'.
                getString(R.string.about_whats_new, getString(R.string.firefox)),
            ),
            AboutPageItem(
                AboutItem.ExternalLink(
                    SUPPORT,
                    SupportUtils.getSumoURLForTopic(context, SupportUtils.SumoTopic.HELP),
                ),
                getString(R.string.about_support),
            ),
            AboutPageItem(
                AboutItem.Crashes,
                getString(R.string.about_crashes),
            ),
            AboutPageItem(
                AboutItem.ExternalLink(
                    PRIVACY_NOTICE,
                    SupportUtils.getMozillaPageUrl(SupportUtils.MozillaPage.PRIVATE_NOTICE),
                ),
                getString(R.string.about_privacy_notice),
            ),
            AboutPageItem(
                AboutItem.ExternalLink(
                    RIGHTS,
                    SupportUtils.getSumoURLForTopic(context, SupportUtils.SumoTopic.YOUR_RIGHTS),
                ),
                getString(R.string.about_know_your_rights),
            ),
            AboutPageItem(
                AboutItem.ExternalLink(LICENSING_INFO, ABOUT_LICENSE_URL),
                getString(R.string.about_licensing_information),
            ),
            AboutPageItem(
                AboutItem.Libraries,
                getString(R.string.about_other_open_source_libraries),
            ),
        )
    }

    private fun openLinkInNormalTab(url: String) {
        (activity as HomeActivity).openToBrowserAndLoad(
            searchTermOrURL = url,
            newTab = true,
            from = BrowserDirection.FromAbout,
        )
    }

    private fun openLibrariesPage() {
        val navController = findNavController()
        navController.navigate(R.id.action_aboutFragment_to_aboutLibrariesFragment)
    }

    override fun onAboutItemClicked(item: AboutItem) {
        when (item) {
            is AboutItem.ExternalLink -> {
                when (item.type) {
                    WHATS_NEW -> {
                        WhatsNew.userViewedWhatsNew(requireContext())
                        Events.whatsNewTapped.record(Events.WhatsNewTappedExtra(source = "ABOUT"))
                    }
                    SUPPORT, PRIVACY_NOTICE, LICENSING_INFO, RIGHTS -> {} // no telemetry needed
                }

                openLinkInNormalTab(item.url)
            }
            is AboutItem.Libraries -> {
                openLibrariesPage()
            }
            is AboutItem.Crashes -> {
                startActivity(Intent(requireContext(), CrashListActivity::class.java))
            }
        }
    }

    companion object {
        private const val ABOUT_LICENSE_URL = "about:license"
    }
}
