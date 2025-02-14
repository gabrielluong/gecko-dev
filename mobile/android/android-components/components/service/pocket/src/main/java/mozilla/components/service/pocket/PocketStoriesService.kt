/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.service.pocket

import android.content.Context
import androidx.annotation.VisibleForTesting
import mozilla.components.service.pocket.PocketStory.ContentRecommendation
import mozilla.components.service.pocket.PocketStory.PocketRecommendedStory
import mozilla.components.service.pocket.PocketStory.PocketSponsoredStory
import mozilla.components.service.pocket.PocketStory.SponsoredContent
import mozilla.components.service.pocket.mars.SponsoredContentsUseCases
import mozilla.components.service.pocket.recommendations.ContentRecommendationsUseCases
import mozilla.components.service.pocket.spocs.SpocsUseCases
import mozilla.components.service.pocket.stories.PocketStoriesUseCases
import mozilla.components.service.pocket.update.ContentRecommendationsRefreshScheduler
import mozilla.components.service.pocket.update.PocketStoriesRefreshScheduler
import mozilla.components.service.pocket.update.SpocsRefreshScheduler
import mozilla.components.service.pocket.update.SponsoredContentsRefreshScheduler

/**
 * Allows for getting a list of pocket stories based on the provided [PocketStoriesConfig]
 *
 * @param context Android Context. Prefer sending application context to limit the possibility of even small leaks.
 * @param pocketStoriesConfig Configuration for how and what pocket stories to get.
 */
class PocketStoriesService(
    private val context: Context,
    private val pocketStoriesConfig: PocketStoriesConfig,
) {
    @VisibleForTesting
    internal var storiesRefreshScheduler = PocketStoriesRefreshScheduler(pocketStoriesConfig)

    @VisibleForTesting
    internal var spocsRefreshscheduler = SpocsRefreshScheduler(pocketStoriesConfig)

    @VisibleForTesting
    internal var contentRecommendationsRefreshScheduler =
        ContentRecommendationsRefreshScheduler(pocketStoriesConfig)

    @VisibleForTesting
    internal var sponsoredContentsRefreshScheduler =
        SponsoredContentsRefreshScheduler(pocketStoriesConfig)

    @VisibleForTesting
    internal var storiesUseCases = PocketStoriesUseCases(
        appContext = context,
        fetchClient = pocketStoriesConfig.client,
    )

    @VisibleForTesting
    internal var spocsUseCases = when (pocketStoriesConfig.profile) {
        null -> {
            logger.debug("Missing profile for sponsored stories")
            null
        }
        else -> SpocsUseCases(
            appContext = context,
            fetchClient = pocketStoriesConfig.client,
            profileId = pocketStoriesConfig.profile.profileId,
            appId = pocketStoriesConfig.profile.appId,
            sponsoredStoriesParams = pocketStoriesConfig.sponsoredStoriesParams,
        )
    }

    @VisibleForTesting
    internal var contentRecommendationsUseCases = ContentRecommendationsUseCases(
        appContext = context,
        client = pocketStoriesConfig.client,
        config = pocketStoriesConfig.contentRecommendationsParams,
    )

    @VisibleForTesting
    internal var sponsoredContentsUseCases = SponsoredContentsUseCases(
        appContext = context,
        client = pocketStoriesConfig.client,
        config = pocketStoriesConfig.marsSponsoredContentsParams,
    )

    /**
     * Entry point to start fetching Pocket stories in the background.
     *
     * Use this at an as high as possible level in your application.
     * Must be paired in a similar way with the [stopPeriodicStoriesRefresh] method.
     *
     * This starts the process of downloading and caching Pocket stories in the background,
     * making them available for the [getStories] method.
     */
    fun startPeriodicStoriesRefresh() {
        GlobalDependencyProvider.RecommendedStories.initialize(storiesUseCases)
        storiesRefreshScheduler.schedulePeriodicRefreshes(context)
    }

    /**
     * Single stopping point for the "get Pocket stories" functionality.
     *
     * Use this at an as high as possible level in your application.
     * Must be paired in a similar way with the [startPeriodicStoriesRefresh] method.
     *
     * This stops the process of downloading and caching Pocket stories in the background.
     */
    fun stopPeriodicStoriesRefresh() {
        storiesRefreshScheduler.stopPeriodicRefreshes(context)
        GlobalDependencyProvider.RecommendedStories.reset()
    }

    /**
     * Get a list of Pocket recommended stories based on the initial configuration.
     *
     * To be called after [startPeriodicStoriesRefresh] to ensure the recommendations are up-to-date.
     * Might return an empty list or a list of older than expected stories if
     * [startPeriodicStoriesRefresh] hasn't yet completed.
     */
    suspend fun getStories(): List<PocketRecommendedStory> {
        return storiesUseCases.getStories()
    }

    /**
     * Entry point to start fetching Pocket sponsored stories in the background.
     *
     * Use this at an as high as possible level in your application.
     * Must be paired in a similar way with the [stopPeriodicSponsoredStoriesRefresh] method.
     *
     * This starts the process of downloading and caching Pocket sponsored stories in the background,
     * making them available for the [getSponsoredStories] method.
     */
    fun startPeriodicSponsoredStoriesRefresh() {
        val useCases = spocsUseCases
        if (useCases == null) {
            logger.warn("Cannot start sponsored stories refresh. Service has incomplete setup")
            return
        }

        GlobalDependencyProvider.SponsoredStories.initialize(useCases)
        spocsRefreshscheduler.stopProfileDeletion(context)
        spocsRefreshscheduler.schedulePeriodicRefreshes(context)
    }

    /**
     * Single stopping point for the "refresh sponsored Pocket stories" functionality.
     *
     * Use this at an as high as possible level in your application.
     * Must be paired in a similar way with the [startPeriodicSponsoredStoriesRefresh] method.
     *
     * This stops the process of downloading and caching Pocket sponsored stories in the background.
     */
    fun stopPeriodicSponsoredStoriesRefresh() {
        spocsRefreshscheduler.stopPeriodicRefreshes(context)
    }

    /**
     * Fetch sponsored Pocket stories and refresh the locally persisted list.
     */
    suspend fun refreshSponsoredStories() {
        spocsUseCases?.refreshStories?.invoke()
    }

    /**
     * Get a list of Pocket sponsored stories based on the initial configuration.
     */
    suspend fun getSponsoredStories(): List<PocketSponsoredStory> {
        return spocsUseCases?.getStories?.invoke() ?: emptyList()
    }

    /**
     * Delete all stored user data used for downloading personalized sponsored stories.
     * This returns immediately but will handle the profile deletion in background.
     */
    fun deleteProfile() {
        val useCases = spocsUseCases
        if (useCases == null) {
            logger.warn("Cannot delete sponsored stories profile. Service has incomplete setup")
            return
        }

        GlobalDependencyProvider.SponsoredStories.initialize(useCases)
        spocsRefreshscheduler.stopPeriodicRefreshes(context)
        spocsRefreshscheduler.scheduleProfileDeletion(context)
    }

    /**
     * Update how many times certain stories were shown to the user.
     *
     * Safe to call from any background thread.
     * Automatically synchronized with the other [PocketStoriesService] methods.
     */
    suspend fun updateStoriesTimesShown(updatedStories: List<PocketRecommendedStory>) {
        storiesUseCases.updateTimesShown(updatedStories)
    }

    /**
     * Persist locally that the sponsored Pocket stories containing the ids from [storiesShown]
     * were shown to the user.
     * This is safe to call with any ids, even ones for stories not currently persisted anymore.
     */
    suspend fun recordStoriesImpressions(storiesShown: List<Int>) {
        spocsUseCases?.recordImpression?.invoke(storiesShown)
    }

    /**
     * Starts a work request in the background to periodically update the list of content
     * recommendations.
     *
     * Use this at an as high as possible level in your application. Must be paired in a similar
     * way with the [stopPeriodicContentRecommendationsRefresh] method.
     *
     * This starts the process of downloading and caching content recommendations in the background
     * and making them available when the [getContentRecommendations] method is called.
     */
    fun startPeriodicContentRecommendationsRefresh() {
        GlobalDependencyProvider.ContentRecommendations.initialize(contentRecommendationsUseCases)
        contentRecommendationsRefreshScheduler.startPeriodicWork(context)
    }

    /**
     * Stops the work request to periodically update the list of content recommendations.
     */
    fun stopPeriodicContentRecommendationsRefresh() {
        contentRecommendationsRefreshScheduler.stopPeriodicWork(context)
        GlobalDependencyProvider.ContentRecommendations.reset()
    }

    /**
     * Returns a list of [ContentRecommendation] based on the initial [pocketStoriesConfig].
     */
    suspend fun getContentRecommendations(): List<ContentRecommendation> {
        return contentRecommendationsUseCases.getContentRecommendations()
    }

    /**
     * Updates the number of impressions (times shown) for a list of [ContentRecommendation]s.
     *
     * @param recommendationsShown The list of [ContentRecommendation]s with updated impressions
     * to persist in storage.
     */
    suspend fun updateRecommendationsImpressions(recommendationsShown: List<ContentRecommendation>) {
        contentRecommendationsUseCases.updateRecommendationsImpressions(recommendationsShown)
    }

    /**
     * Starts a work request in the background to periodically refresh the list of sponsored
     * contents.
     *
     * Use this at an as high as possible level in your application. Must be paired in a similar
     * way with the [stopPeriodicSponsoredContentsRefresh] method.
     *
     * This starts the process of downloading and caching sponsored contents in the background
     * and making them available when the [getSponsoredContents] method is called.
     */
    fun startPeriodicSponsoredContentsRefresh() {
        GlobalDependencyProvider.SponsoredContents.initialize(sponsoredContentsUseCases)
        sponsoredContentsRefreshScheduler.startPeriodicRefreshes(context)
    }

    /**
     * Stops the work request to periodically refresh the list of sponsored contents.
     */
    fun stopPeriodicSponsoredContentsRefresh() {
        sponsoredContentsRefreshScheduler.stopPeriodicRefreshes(context)
        GlobalDependencyProvider.SponsoredContents.reset()
    }

    /**
     * Returns a list of [SponsoredContent] based on the initial [pocketStoriesConfig].
     */
    suspend fun getSponsoredContents(): List<SponsoredContent> {
        return sponsoredContentsUseCases.getSponsoredContents()
    }

    /**
     * Records the sponsored content impressions from the provided list of sponsored content
     * URLs.
     *
     * @param impressions A list of sponsored content URLs that have been viewed.
     */
    suspend fun recordSponsoredContentImpressions(impressions: List<String>) {
        sponsoredContentsUseCases.recordImpressions.invoke(impressions)
    }

    /**
     * Deletes all data persisted for sponsored content.
     * This returns immediately but will handle the profile deletion in background.
     */
    fun deleteUser() {
        GlobalDependencyProvider.SponsoredContents.initialize(sponsoredContentsUseCases)
        sponsoredContentsRefreshScheduler.stopPeriodicRefreshes(context)
        sponsoredContentsRefreshScheduler.scheduleUserDeletion(context)
    }
}
