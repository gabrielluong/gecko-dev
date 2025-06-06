/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.samples.browser

import android.app.Application
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch
import mozilla.components.browser.state.action.SystemAction
import mozilla.components.browser.storage.sync.GlobalPlacesDependencyProvider
import mozilla.components.feature.addons.update.GlobalAddonDependencyProvider
import mozilla.components.lib.fetch.httpurlconnection.HttpURLConnectionClient
import mozilla.components.service.glean.net.ConceptFetchHttpUploader
import mozilla.components.support.base.facts.Facts
import mozilla.components.support.base.facts.processor.LogFactProcessor
import mozilla.components.support.base.log.Log
import mozilla.components.support.base.log.logger.Logger
import mozilla.components.support.base.log.sink.AndroidLogSink
import mozilla.components.support.ktx.android.content.isMainProcess
import mozilla.components.support.ktx.android.content.runOnlyInMainProcess
import mozilla.components.support.rustlog.RustLog
import mozilla.components.support.webextensions.WebExtensionSupport
import mozilla.telemetry.glean.BuildInfo
import mozilla.telemetry.glean.Glean
import mozilla.telemetry.glean.config.Configuration
import java.util.Calendar
import java.util.TimeZone
import java.util.concurrent.TimeUnit

@Suppress("MagicNumber")
internal object GleanBuildInfo {
    val buildInfo: BuildInfo by lazy {
        BuildInfo(
            versionCode = "0.0.1",
            versionName = "0.0.1",
            buildDate = Calendar.getInstance(
                TimeZone.getTimeZone("GMT+0"),
            ).also { cal -> cal.set(2019, 9, 23, 12, 52, 8) },
        )
    }
}

class SampleApplication : Application() {
    private val logger = Logger("SampleApplication")

    val components by lazy { Components(this) }

    @OptIn(DelicateCoroutinesApi::class) // Usage of GlobalScope
    override fun onCreate() {
        super.onCreate()

        RustLog.enable()

        Log.addSink(AndroidLogSink())

        components.crashReporter.install(this)

        if (!isMainProcess()) {
            return
        }

        val httpClient = ConceptFetchHttpUploader(lazy { HttpURLConnectionClient() })
        val config = Configuration(httpClient = httpClient)
        // IMPORTANT: the following lines initialize the Glean SDK but disable upload
        // of pings. If, for testing purposes, upload is required to be on, change the
        // next line to `uploadEnabled = true`.
        Glean.initialize(
            applicationContext,
            uploadEnabled = false,
            configuration = config,
            buildInfo = GleanBuildInfo.buildInfo,
        )

        Facts.registerProcessor(LogFactProcessor())

        components.engine.warmUp()
        restoreBrowserState()

        GlobalScope.launch(Dispatchers.IO) {
            components.webAppManifestStorage.warmUpScopes(System.currentTimeMillis())
        }
        components.downloadsUseCases.restoreDownloads()
        try {
            GlobalPlacesDependencyProvider.initialize(components.historyStorage)
            GlobalAddonDependencyProvider.initialize(
                components.addonManager,
                components.addonUpdater,
            )
            WebExtensionSupport.initialize(
                components.engine,
                components.store,
                onNewTabOverride = {
                        _, engineSession, url ->
                    components.tabsUseCases.addTab(url, selectTab = true, engineSession = engineSession)
                },
                onCloseTabOverride = {
                        _, sessionId ->
                    components.tabsUseCases.removeTab(sessionId)
                },
                onSelectTabOverride = {
                        _, sessionId ->
                    components.tabsUseCases.selectTab(sessionId)
                },
                onUpdatePermissionRequest = components.addonUpdater::onUpdatePermissionRequest,
                onExtensionsLoaded = { extensions ->
                    components.addonUpdater.registerForFutureUpdates(extensions)
                    components.supportedAddonsChecker.registerForChecks()
                },
            )
        } catch (e: UnsupportedOperationException) {
            // Web extension support is only available for engine gecko
            Logger.error("Failed to initialize web extension support", e)
        }
    }

    @DelicateCoroutinesApi
    private fun restoreBrowserState() = GlobalScope.launch(Dispatchers.Main) {
        components.tabsUseCases.restore(components.sessionStorage)

        components.sessionStorage.autoSave(components.store)
            .periodicallyInForeground(interval = 30, unit = TimeUnit.SECONDS)
            .whenGoingToBackground()
            .whenSessionsChange()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)

        logger.debug("onTrimMemory: $level")

        runOnlyInMainProcess {
            components.store.dispatch(SystemAction.LowMemoryAction(level))

            components.icons.onTrimMemory(level)
        }
    }
}
