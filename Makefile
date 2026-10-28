# =============================================================================
# Makefile for Shem (Author: Jáchym Pešek)
# =============================================================================

ANDROID_HOME ?= $(HOME)/Android/Sdk
JAVA_HOME ?= /usr/lib/jvm/java-17-openjdk

all: release-apk

apk:
	cd android && ANDROID_HOME=$(ANDROID_HOME) JAVA_HOME=$(JAVA_HOME) ./gradlew assembleDebug --no-daemon
	cp android/app/build/outputs/apk/debug/app-debug.apk shem.apk
	@echo "Built shem.apk successfully."

release-apk:
	cd android && ANDROID_HOME=$(ANDROID_HOME) JAVA_HOME=$(JAVA_HOME) ./gradlew assembleRelease --no-daemon
	cp android/app/build/outputs/apk/release/app-release.apk shem-release.apk
	@echo "Built shem-release.apk successfully."

install: apk
	adb install -r shem.apk

install-release: release-apk
	adb install -r shem-release.apk

clean:
	cd android && ANDROID_HOME=$(ANDROID_HOME) JAVA_HOME=$(JAVA_HOME) ./gradlew clean --no-daemon
	rm -f shem.apk shem-release.apk

.PHONY: all apk release-apk install install-release clean
