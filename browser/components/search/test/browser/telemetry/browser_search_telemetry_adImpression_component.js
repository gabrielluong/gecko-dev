/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const WINDOW_HEIGHT = 768;
const WINDOW_WIDTH = 1024;

// Note: example.org is used for the SERP page, and example.com is used to serve
// the ads. This is done to simulate different domains like the real servers.
const TEST_PROVIDER_INFO = [
  {
    telemetryId: "example",
    searchPageRegexp:
      /^https:\/\/example.org\/browser\/browser\/components\/search\/test\/browser\/telemetry\/searchTelemetryAd/,
    queryParamNames: ["s"],
    codeParamName: "abc",
    taggedCodes: ["ff"],
    adServerAttributes: ["mozAttr"],
    extraAdServersRegexps: [/^https:\/\/example\.com\/ad/],
    components: [
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
        included: {
          parent: {
            selector: ".moz-carousel",
          },
          children: [
            {
              selector: ".moz-carousel-card",
              countChildren: true,
            },
          ],
        },
      },
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.REFINED_SEARCH_BUTTONS,
        included: {
          parent: {
            selector: ".refined-search-buttons",
          },
          children: [
            {
              selector: "a",
            },
          ],
        },
        topDown: true,
      },
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.AD_LINK,
        included: {
          parent: {
            selector: ".moz_ad",
          },
          children: [
            {
              selector: ".multi-col",
              type: SearchSERPTelemetryUtils.COMPONENTS.AD_SITELINK,
            },
          ],
        },
        excluded: {
          parent: {
            selector: ".rhs",
          },
        },
      },
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.AD_SIDEBAR,
        included: {
          parent: {
            selector: ".rhs",
          },
        },
      },
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.AD_LINK,
        default: true,
      },
      {
        type: SearchSERPTelemetryUtils.COMPONENTS.COOKIE_BANNER,
        included: {
          parent: {
            selector: "#banner",
          },
        },
        topDown: true,
      },
    ],
  },
];

add_setup(async function () {
  SearchSERPTelemetry.overrideSearchTelemetryForTests(TEST_PROVIDER_INFO);
  await waitForIdle();
  // Enable local telemetry recording for the duration of the tests.
  let oldCanRecord = Services.telemetry.canRecordExtended;
  Services.telemetry.canRecordExtended = true;

  // The tests evaluate whether or not ads are visible depending on whether
  // they are within the view of the window. To ensure the test results
  // are consistent regardless of where they are launched,
  // set the window size to something reasonable.
  let originalWidth = window.outerWidth;
  let originalHeight = window.outerHeight;
  await resizeWindow(window, WINDOW_WIDTH, WINDOW_HEIGHT);

  registerCleanupFunction(async () => {
    SearchSERPTelemetry.overrideSearchTelemetryForTests();
    Services.telemetry.canRecordExtended = oldCanRecord;
    await resizeWindow(window, originalWidth, originalHeight);
    resetTelemetry();
  });
});

add_task(async function test_ad_impressions_with_one_carousel() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_carousel.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "4",
          ads_visible: "3",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

// This is to ensure we're not counting two carousel components as two
// separate components but as one record with a sum of the results.
add_task(async function test_ad_impressions_with_two_carousels() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_carousel_doubled.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  // This is to ensure we've seen the other carousel regardless the
  // size of the browser window.
  await SpecialPowers.spawn(gBrowser.selectedBrowser, [], () => {
    let el = content.document
      .getElementById("second-ad")
      .getBoundingClientRect();
    // The 100 is just to guarantee we've scrolled past the element.
    content.scrollTo(0, el.top + el.height + 100);
  });

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "8",
          ads_visible: "6",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(
  async function test_ad_impressions_with_carousels_with_outer_container() {
    resetTelemetry();
    let url = getSERPUrl(
      "searchTelemetryAd_components_carousel_outer_container.html"
    );
    let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

    await waitForPageWithAdImpressions();

    // A common pattern for carousels is for one element to be the mask
    // while a child element contains all individual elements. If we select the
    // parent selector of an element that isn't the mask and is the container
    // of all elements, we'll potentially end up counting all elements as
    // visible.
    assertSERPTelemetry([
      {
        impression: {
          provider: "example",
          tagged: "true",
          partner_code: "ff",
          source: "unknown",
          is_shopping_page: "false",
          is_private: "false",
          shopping_tab_displayed: "false",
          is_signed_in: "false",
        },
        adImpressions: [
          {
            component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
            ads_loaded: "4",
            ads_visible: "4",
            ads_hidden: "0",
          },
        ],
      },
    ]);

    BrowserTestUtils.removeTab(tab);
  }
);

add_task(async function test_ad_impressions_with_carousels_tabhistory() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_carousel.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);
  await waitForPageWithAdImpressions();

  let browserLoadedPromise = BrowserTestUtils.browserLoaded(tab.linkedBrowser);
  BrowserTestUtils.startLoadingURIString(
    tab.linkedBrowser,
    "https://www.example.com/some_url"
  );
  await browserLoadedPromise;

  // Reset telemetry because we care about the telemetry upon going back.
  resetTelemetry();

  let pageShowPromise = BrowserTestUtils.waitForContentEvent(
    tab.linkedBrowser,
    "pageshow"
  );
  tab.linkedBrowser.goBack();
  await pageShowPromise;

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "tabhistory",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "4",
          ads_visible: "3",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_ad_impressions_with_hidden_carousels() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_carousel_hidden.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "4",
          ads_visible: "0",
          ads_hidden: "4",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_ad_impressions_with_carousel_scrolled_left() {
  resetTelemetry();
  let url = getSERPUrl(
    "searchTelemetryAd_components_carousel_first_element_non_visible.html"
  );
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "4",
          ads_visible: "3",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_ad_impressions_with_carousel_below_the_fold() {
  resetTelemetry();
  let url = getSERPUrl(
    "searchTelemetryAd_components_carousel_below_the_fold.html"
  );
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
          ads_loaded: "4",
          ads_visible: "0",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

// This test takes a carousel that's below the fold and places it at the fold
// to check that when the ad impression is recorded,
add_task(
  async function test_ad_impressions_visibility_with_carousel_at_the_fold() {
    resetTelemetry();
    let url = getSERPUrl(
      "searchTelemetryAd_components_carousel_below_the_fold.html"
    );
    let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

    // Position the carousel such that it only partially appears.
    // The amount that should show depends on VISIBILITY_THRESHOLD.
    await SpecialPowers.spawn(
      gBrowser.selectedBrowser,
      [VISIBILITY_THRESHOLD],
      threshold => {
        let el = content.document.querySelector(".moz-carousel-container");

        let dimensions = el.getBoundingClientRect();
        let adjustedHeight = dimensions.height * threshold;
        // This is a tiny amount of added visibility as a precautionary
        // measure to avoid intermittent failures.
        let adjustment = 2;

        let top = content.window.innerHeight - adjustedHeight - adjustment;
        el.style.position = "absolute";
        el.style.top = `${top}px`;
      }
    );

    await waitForPageWithAdImpressions();

    assertSERPTelemetry([
      {
        impression: {
          provider: "example",
          tagged: "true",
          partner_code: "ff",
          source: "unknown",
          is_shopping_page: "false",
          is_private: "false",
          shopping_tab_displayed: "false",
          is_signed_in: "false",
        },
        adImpressions: [
          {
            component: SearchSERPTelemetryUtils.COMPONENTS.AD_CAROUSEL,
            ads_loaded: "4",
            // The fourth ad is not viewable because it requires the user
            // to scroll the carousel.
            ads_visible: "3",
            ads_hidden: "0",
          },
        ],
      },
    ]);

    BrowserTestUtils.removeTab(tab);
  }
);

add_task(async function test_ad_impressions_with_text_links() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_text.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_SITELINK,
          ads_loaded: "1",
          ads_visible: "1",
          ads_hidden: "0",
        },
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_LINK,
          ads_loaded: "2",
          ads_visible: "2",
          ads_hidden: "0",
        },
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_SIDEBAR,
          ads_loaded: "1",
          ads_visible: "1",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

// An ad is considered visible if at least one link is within the viewable
// content area when the impression was taken. Since the user can scroll
// the page before ad impression is recorded, we should ensure that an
// ad that was scrolled onto the screen before the impression is taken is
// properly recorded. Additionally, some ads might have a large content
// area that extends beyond the viewable area, but as long as a single
// ad link was viewable within the area, we should count the ads as visible.
add_task(async function test_ad_visibility() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_visibility.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await SpecialPowers.spawn(gBrowser.selectedBrowser, [], () => {
    let el = content.document.getElementById("second-ad");
    // There's top padding on the element to push it far down the page, so to
    // ensure that we actually snapshot the ad, we want to scroll to the bottom
    // of it.
    el.scrollIntoView(false);
  });

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.AD_LINK,
          ads_loaded: "6",
          ads_visible: "4",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_impressions_without_ads() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_searchbox_with_content.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.REFINED_SEARCH_BUTTONS,
          ads_loaded: "1",
          ads_visible: "1",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_ad_impressions_with_cookie_banner() {
  resetTelemetry();
  let url = getSERPUrl("searchTelemetryAd_components_cookie_banner.html");
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, url);

  await waitForPageWithAdImpressions();

  assertSERPTelemetry([
    {
      impression: {
        provider: "example",
        tagged: "true",
        partner_code: "ff",
        source: "unknown",
        is_shopping_page: "false",
        is_private: "false",
        shopping_tab_displayed: "false",
        is_signed_in: "false",
      },
      adImpressions: [
        {
          component: SearchSERPTelemetryUtils.COMPONENTS.COOKIE_BANNER,
          ads_loaded: "1",
          ads_visible: "1",
          ads_hidden: "0",
        },
      ],
    },
  ]);

  BrowserTestUtils.removeTab(tab);
});
