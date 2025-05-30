/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix

import kotlinx.coroutines.Job
import mozilla.components.lib.state.Action
import mozilla.components.lib.state.State
import mozilla.components.lib.state.Store
import mozilla.components.service.pocket.PocketStory.ContentRecommendation
import mozilla.components.support.test.ext.joinBlocking
import mozilla.components.support.test.libstate.ext.waitUntilIdle

/**
 * Utility file for providing shared functions that are used across multiple test files.
 */
object TestUtils {

    /**
     * Creates and returns a list of [ContentRecommendation]s.
     *
     * @param limit Number of [ContentRecommendation]s to create.
     */
    fun getFakeContentRecommendations(
        limit: Int = 1,
    ): List<ContentRecommendation> {
        return mutableListOf<ContentRecommendation>().apply {
            for (index in 0 until limit) {
                add(
                    ContentRecommendation(
                        corpusItemId = "corpusItemId$index",
                        scheduledCorpusItemId = "scheduledCorpusItemId$index",
                        url = "https://story$index.com",
                        title = "Recommendation - This is a ${"very ".repeat(index)} long title",
                        excerpt = "Excerpt",
                        topic = "topic$index",
                        publisher = "Publisher",
                        isTimeSensitive = false,
                        imageUrl = "",
                        tileId = index.toLong(),
                        receivedRank = index,
                        recommendedAt = index.toLong(),
                        impressions = index.toLong(),
                    ),
                )
            }
        }
    }
}

/**
 * Blocking [Store.dispatch] call of the given [action] ensures completion of the [Job].
 */
fun <S : State, A : Action> Store<S, A>.testDispatch(action: A) {
    dispatch(action).joinBlocking()
    waitUntilIdle()
}
