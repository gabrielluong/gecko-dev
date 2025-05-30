import json
import os
import time

import mozinstall
import mozversion
import pytest
import requests
from mozdownload import DirectScraper, FactoryScraper
from mozprofile import Profile

from .gradlewbuild import GradlewBuild
from .tps import TPS

here = os.path.dirname(__file__)


@pytest.fixture(scope="session")
def firefox(pytestconfig, tmpdir_factory):
    binary = os.getenv("MOZREGRESSION_BINARY", pytestconfig.getoption("firefox"))
    if binary is None:
        cache_dir = str(pytestconfig.cache.makedir("firefox"))
        scraper = FactoryScraper("daily", destination=cache_dir)
        build_path = scraper.download()
        install_path = str(tmpdir_factory.mktemp("firefox"))
        install_dir = mozinstall.install(src=build_path, dest=install_path)
        binary = mozinstall.get_binary(install_dir, "firefox")
    version = mozversion.get_version(binary)
    if hasattr(pytestconfig, "_metadata"):
        pytestconfig._metadata.update(version)
    return binary


@pytest.fixture
def firefox_log(pytestconfig, tmpdir):
    firefox_log = str(tmpdir.join("firefox.log"))
    pytestconfig._firefox_log = firefox_log
    yield firefox_log


@pytest.fixture(scope="session")
def tps_addon(pytestconfig, tmpdir_factory):
    path = pytestconfig.getoption("tps")
    if path is not None:
        return path
    task_url = (
        "https://firefox-ci-tc.services.mozilla.com/api/index/v1/task/"
        "gecko.v2.mozilla-central.latest.firefox.addons.tps"
    )
    task_id = requests.get(task_url).json().get("taskId")
    cache_dir = str(pytestconfig.cache.makedir(f"tps-{task_id}"))
    addon_url = (
        "https://firefox-ci-tc.services.mozilla.com/api/queue/v1/task/"
        f"{task_id}/artifacts/public/tps.xpi"
    )
    scraper = DirectScraper(addon_url, destination=cache_dir)
    return scraper.download()


@pytest.fixture
def tps_config(fxa_account, monkeypatch):
    monkeypatch.setenv("FXA_EMAIL", fxa_account.email)
    monkeypatch.setenv("FXA_PASSWORD", fxa_account.password)

    # Go to resources folder
    os.chdir("../../../../..")
    resources = r"resources"
    resourcesDir = os.path.join(os.getcwd(), resources)

    with open(os.path.join(resourcesDir, "email.txt"), "w") as f:
        f.write(fxa_account.email)

    with open(os.path.join(resourcesDir, "password.txt"), "w") as f:
        f.write(fxa_account.password)

    # Set the path where tests are
    os.chdir("../")
    currentDir = os.getcwd()
    testsDir = currentDir + "/androidTest/java/org/mozilla/fenix/syncintegration"
    os.chdir(testsDir)

    yield {
        "fx_account": {"username": fxa_account.email, "password": fxa_account.password}
    }


@pytest.fixture
def tps_log(pytestconfig, tmpdir):
    tps_log = str(tmpdir.join("tps.log"))
    pytestconfig._tps_log = tps_log
    yield tps_log


@pytest.fixture
def tps_profile(pytestconfig, tps_addon, tps_config, tps_log, fxa_urls):
    preferences = {
        "app.update.enabled": False,
        "browser.dom.window.dump.enabled": True,
        "browser.onboarding.enabled": False,
        "browser.sessionstore.resume_from_crash": False,
        "browser.shell.checkDefaultBrowser": False,
        "browser.startup.homepage_override.mstone": "ignore",
        "browser.startup.page": 0,
        "browser.tabs.warnOnClose": False,
        "browser.warnOnQuit": False,
        "datareporting.policy.dataSubmissionEnabled": False,
        # 'devtools.chrome.enabled': True,
        # 'devtools.debugger.remote-enabled': True,
        "engine.bookmarks.repair.enabled": False,
        "extensions.autoDisableScopes": 10,
        "extensions.experiments.enabled": True,
        "extensions.update.enabled": False,
        "extensions.update.notifyUser": False,
        # While this line is commented prod is launched instead of stage
        "identity.fxaccounts.autoconfig.uri": fxa_urls["content"],
        "testing.tps.skipPingValidation": True,
        "services.sync.firstSync": "notReady",
        "services.sync.lastversion": "1.0",
        "services.sync.log.appender.console": "Trace",
        "services.sync.log.appender.dump": "Trace",
        "services.sync.log.appender.file.level": "Trace",
        "services.sync.log.appender.file.logOnSuccess": True,
        "services.sync.log.logger": "Trace",
        "services.sync.log.logger.engine": "Trace",
        "services.sync.testing.tps": True,
        "testing.tps.logFile": tps_log,
        "toolkit.startup.max_resumed_crashes": -1,
        "tps.config": json.dumps(tps_config),
        "tps.seconds_since_epoch": int(time.time()),
        "xpinstall.signatures.required": False,
    }
    profile = Profile(addons=[tps_addon], preferences=preferences)
    pytestconfig._profile = profile.profile
    yield profile


@pytest.fixture
def tps(firefox, firefox_log, monkeypatch, pytestconfig, tps_log, tps_profile):
    yield TPS(firefox, firefox_log, tps_log, tps_profile)


@pytest.fixture
def gradlewbuild_log(pytestconfig, tmpdir):
    gradlewbuild_log = str(tmpdir.join("gradlewbuild.log"))
    pytestconfig._gradlewbuild_log = gradlewbuild_log
    yield gradlewbuild_log


@pytest.fixture
def gradlewbuild(fxa_account, monkeypatch, gradlewbuild_log):
    monkeypatch.setenv("FXA_EMAIL", fxa_account.email)
    monkeypatch.setenv("FXA_PASSWORD", fxa_account.password)
    yield GradlewBuild(gradlewbuild_log)


def pytest_addoption(parser):
    parser.addoption(
        "--firefox",
        help="path to firefox binary (defaults to " "downloading latest nightly build)",
    )
    parser.addoption(
        "--tps",
        help="path to tps add-on (defaults to " "downloading latest nightly build)",
    )


@pytest.mark.hookwrapper
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    extra = getattr(report, "extra", [])
    pytest_html = item.config.pluginmanager.getplugin("html")
    profile = getattr(item.config, "_profile", None)
    if profile is not None and os.path.exists(profile):
        # add sync logs to HTML report
        for root, _, files in os.walk(os.path.join(profile, "weave", "logs")):
            for f in files:
                path = os.path.join(root, f)
                if pytest_html is not None:
                    with open(path, encoding="utf8") as f:
                        extra.append(pytest_html.extras.text(f.read(), "Sync"))
                report.sections.append(("Sync", f"Log: {path}"))
    for log in ("Firefox", "TPS", "GradlewBuild"):
        attr = f"_{log.lower()}_log"
        path = getattr(item.config, attr, None)
        if path is not None and os.path.exists(path):
            if pytest_html is not None:
                with open(path, encoding="utf8") as f:
                    extra.append(pytest_html.extras.text(f.read(), log))
            report.sections.append((log, f"Log: {path}"))
    report.extra = extra
