// |reftest| skip-if(!this.hasOwnProperty('Intl'))

const tests = {
  "en": {
    long: {
      "de": "German",
      "de-AT": "Austrian German",
      "de-1996": "German (German orthography of 1996)",
      "en": "English",
      "en-Hant-GB": "English (Traditional, United Kingdom)",
      "en-Hans-US": "English (Simplified, United States)",
      "fr": "French",
      "nl-BE": "Flemish",
      "cr-Cans": "Cree (Unified Canadian Aboriginal Syllabics)",
    },
    short: {
      "en-Hant-GB": "English (Traditional, UK)",
      "en-Hans-US": "English (Simplified, US)",
      "cr-Cans": "Cree (UCAS)",
    },
    narrow: {},
  },
  "de": {
    long: {
      "de": "Deutsch",
      "de-AT": "Österreichisches Deutsch",
      "de-1996": "Deutsch (Neue deutsche Rechtschreibung)",
      "en": "Englisch",
      "en-Hant-GB": "Englisch (Traditionell, Vereinigtes Königreich)",
      "en-Hans-US": "Englisch (Vereinfacht, Vereinigte Staaten)",
      "fr": "Französisch",
      "nl-BE": "Flämisch",
    },
    short: {
      "en-Hant-GB": "Englisch (Traditionell, UK)",
      "en-Hans-US": "Englisch (Vereinfacht, USA)",
    },
    narrow: {},
  },
  "fr": {
    long: {
      "de": "allemand",
      "de-AT": "allemand autrichien",
      "de-1996": "allemand (orthographe allemande de 1996)",
      "en": "anglais",
      "en-Hant-GB": "anglais (traditionnel, Royaume-Uni)",
      "en-Hans-US": "anglais (simplifié, États-Unis)",
      "fr": "français",
      "nl-BE": "flamand",
    },
    short: {
      "en-Hant-GB": "anglais (traditionnel, R.-U.)",
      "en-Hans-US": "anglais (simplifié, É.-U.)",
    },
    narrow: {},
  },
  "zh": {
    long: {
      "zh": "中文",
      "zh-Hant": "繁体中文",
      "zh-Hant-CN": "中文（繁体，中国）",
      "zh-Hans-HK": "中文（简体，中国香港特别行政区）",
    },
    short: {
      "zh-Hans-HK": "中文（简体，香港）",
    },
    narrow: {},
  },
  "ar": {
    long: {
      "ar": "العربية",
      "ar-SA": "العربية (المملكة العربية السعودية)",
      "zh-MO": "الصينية (منطقة ماكاو الإدارية الخاصة)",
    },
    short: {
      "zh-MO": "الصينية (مكاو)",
    },
    narrow: {},
  },
};

for (let [locale, localeTests] of Object.entries(tests)) {
  for (let [style, styleTests] of Object.entries(localeTests)) {
    let dn = new Intl.DisplayNames(locale, {type: "language", languageDisplay: "dialect", style});

    let resolved = dn.resolvedOptions();
    assertEq(resolved.locale, locale);
    assertEq(resolved.style, style);
    assertEq(resolved.type, "language");
    assertEq(resolved.languageDisplay, "dialect");
    assertEq(resolved.fallback, "code");

    let inheritedTests = {...localeTests.long, ...localeTests.short, ...localeTests.narrow};
    for (let [language, expected] of Object.entries({...inheritedTests, ...styleTests})) {
      assertEq(dn.of(language), expected);

      // Also works with objects.
      assertEq(dn.of(Object(language)), expected);
    }
  }
}

if (typeof reportCompare === "function")
  reportCompare(true, true);
