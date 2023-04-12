Notes: the Python virtualenv that runs `mach cramtest` must not be the same
virtualenv that runs `mach build`.

Gradle's pretty output handling interacts badly with the shell itself (as used
by cram, but even outside of cram).  The best thing I found was to use a
subshell, background it, and then immediately wait.  That keeps Gradle from
monkeying with stdout, etc in such a way that the cram shell exits without
running following commands.

  $ export MOZ_OBJDIR=objdir-cram

  $ cd $TESTDIR/../../../../
  $ cat >mozconfig-cram <<-HERE
  > 	ac_add_options --enable-application=mobile/android
  > 	ac_add_options --target=arm
  > 	ac_add_options --enable-artifact-builds
  > 	mk_add_options MOZ_OBJDIR="$MOZ_OBJDIR"
  > HERE
  $ export MOZCONFIG=mozconfig-cram
  $ export MACH_NO_TERMINAL_FOOTER=1
  $ export MACH_NO_WRITE_TIMES=1

When building within `mach`, the `machBuild*` tasks are skipped:

  $ export OUT=out1
  $ (./mach build 2>&1 | tee $OUT > /dev/null) & wait $!

  $ grep ':machBuildFaster' $OUT
  . Task :machBuildFaster SKIPPED (re)
  Skipping task :machBuildFaster because: within `mach build`

  $ grep ':machStagePackage' $OUT
  . Task :machStagePackage SKIPPED (re)
  Skipping task :machStagePackage because: within `mach build`

  $ export GVE_ORIG=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview_example/outputs/apk/withGeckoBinaries/debug/geckoview_example-withGeckoBinaries-debug.apk)
  $ export AT_ORIG=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk)
  $ export OMNIJAR_ORIG=$(unzip -p $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk assets/omni.ja | shasum)

When building from Gradle (i.e., from Android Studio), the `machBuild*` tasks
are not skipped:

  $ export OUT=out2
  $ (./mach gradle geckoview_example:assembleWithGeckoBinariesDebug 2>&1 | tee $OUT > /dev/null) & wait $!

Nothing has changed:

  $ export GVE_NEW=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview_example/outputs/apk/withGeckoBinaries/debug/geckoview_example-withGeckoBinaries-debug.apk)
  $ export AT_NEW=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk)
  $ export OMNIJAR_NEW=$(unzip -p $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk assets/omni.ja | shasum)
  $ test "$GVE_ORIG" == "$GVE_NEW"
  $ test "$AT_ORIG" == "$AT_NEW"
  $ test "$OMNIJAR_ORIG" == "$OMNIJAR_NEW"

N.b.: order matters, since `mach build stage-package` depends on the outputs of
`mach build faster`.

  $ grep 'Executing task :mach' $OUT
  Executing task :machBuildFaster
  Executing task :machStagePackage

Patching the GeckoView main source (the exact file does not matter to this test)
results in new APKs:

  $ patch -p1 <$TESTDIR/geckoviewMain.patch
  patching file mobile/android/geckoview/src/main/java/org/mozilla/geckoview/GeckoView.java
  $ export OUT=out3
  $ (./mach build 2>&1 | tee $OUT > /dev/null) & wait $!
  $ export GVE_NEW=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview_example/outputs/apk/withGeckoBinaries/debug/geckoview_example-withGeckoBinaries-debug.apk)
  $ test "$GVE_ORIG" != "$GVE_NEW"
  $ patch -p1 --reverse <$TESTDIR/geckoviewMain.patch
  patching file mobile/android/geckoview/src/main/java/org/mozilla/geckoview/GeckoView.java

  $ patch -p1 <$TESTDIR/geckoviewMain.patch
  patching file mobile/android/geckoview/src/main/java/org/mozilla/geckoview/GeckoView.java
  $ export OUT=out3
  $ (./mach gradle geckoview_example:assembleWithGeckoBinariesDebug 2>&1 | tee $OUT > /dev/null) & wait $!
  $ export GVE_NEW=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview_example/outputs/apk/withGeckoBinaries/debug/geckoview_example-withGeckoBinaries-debug.apk)
  $ test "$GVE_ORIG" != "$GVE_NEW"
  $ patch -p1 --reverse <$TESTDIR/geckoviewMain.patch
  patching file mobile/android/geckoview/src/main/java/org/mozilla/geckoview/GeckoView.java

Patching the GeckoView androidTest source (the exact file does not matter to
this test) results in new APKs:

  $ patch -p1 <$TESTDIR/geckoviewAndroidTest.patch
  patching file mobile/android/geckoview/src/androidTest/java/org/mozilla/geckoview/test/util/RuntimeCreator.java
  $ export OUT=out3
  $ (./mach build 2>&1 | tee $OUT > /dev/null) & wait $!
  $ export AT_NEW=$(shasum $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk)
  $ test "$AT_ORIG" != "$AT_NEW"
  $ patch -p1 --reverse <$TESTDIR/geckoviewAndroidTest.patch
  patching file mobile/android/geckoview/src/androidTest/java/org/mozilla/geckoview/test/util/RuntimeCreator.java

Patching Gecko resources results in a new omnijar, both in the GeckoView AAR and
in relevant APKs:

  $ patch -p1 <$TESTDIR/modules.patch
  patching file mobile/android/modules/geckoview/GeckoViewUtils.sys.mjs
  $ export OUT=out3
  $ (./mach build 2>&1 | tee $OUT > /dev/null) & wait $!
  $ export OMNIJAR_NEW=$(unzip -p $MOZ_OBJDIR/gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk assets/omni.ja | shasum)
  $ test "$OMNIJAR_ORIG" != "$OMNIJAR_NEW"
  $ patch -p1 --reverse <$TESTDIR/modules.patch
  patching file mobile/android/modules/geckoview/GeckoViewUtils.sys.mjs


# $ grep gradlew.*generateJNIWrappers $TMP/out1
# */gradlew geckoview:generateJNIWrappersForGeneratedWithGeckoBinariesDebug app:generateJNIWrappersForFennecWithoutGeckoBinariesDebug app:assembleWithoutGeckoBinariesDebug app:assembleWithoutGeckoBinariesDebugAndroidTest -x lint (glob)

# $ (./mach gradle app:assembleWithoutGeckoBinariesDebug 2>&1 | tee $TMP/out2 > /dev/null) & wait $!
# $ grep ':machBuildGeneratedAndroidCodeAndResources' $TMP/out2
# . Task :machBuildGeneratedAndroidCodeAndResources (re)
# Executing task :machBuildGeneratedAndroidCodeAndResources
# :machBuildGeneratedAndroidCodeAndResources> /usr/bin/make -C mobile/android/base -j12 -s generated_android_code_and_resources
# :machBuildGeneratedAndroidCodeAndResources> mobile/android/base/generated_android_code_and_resources.stub
# :machBuildGeneratedAndroidCodeAndResources> 0 compiler warnings present.
# :machBuildGeneratedAndroidCodeAndResources> Your build was successful!
