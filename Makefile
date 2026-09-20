# Sona — build everything from the command line.
#
#   make driver        build Driver/build/SonaDriver.driver
#   make app           build App/build/Sona.app
#   make               both
#   sudo make install-driver   copy the driver into /Library/Audio/Plug-Ins/HAL and restart coreaudiod
#   sudo make uninstall-driver
#   make run           launch the app
#
# Requires Xcode command line tools. No Xcode project is needed.

ROOT        := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DRIVER_DIR  := $(ROOT)/Driver
APP_DIR     := $(ROOT)/App
DRIVER_OUT  := $(DRIVER_DIR)/build/SonaDriver.driver
APP_OUT     := $(APP_DIR)/build/Sona.app
HAL_DIR     := /Library/Audio/Plug-Ins/HAL

ARCHS       ?= -arch arm64 -arch x86_64
MIN_MACOS   ?= 14.0
SDK         := $(shell xcrun --show-sdk-path)
CXX         := $(shell xcrun -f clang++)
SWIFT_BUILD := swift build

DRIVER_SRCS := $(DRIVER_DIR)/SonaDriver.cpp $(DRIVER_DIR)/SonaTransportClient.cpp
DRIVER_HDRS := $(DRIVER_DIR)/SonaTransportClient.h $(ROOT)/Shared/SonaProtocol.h $(ROOT)/Shared/SonaTransport.h

SERVICE_DIR   := $(ROOT)/Service
SERVICE_OUT   := $(SERVICE_DIR)/build/SonaAudioService
SERVICE_LABEL := com.sona.audio-service
SERVICE_BIN   := /Library/PrivilegedHelperTools/SonaAudioService
SERVICE_PLIST := /Library/LaunchDaemons/$(SERVICE_LABEL).plist

CXXFLAGS    := -std=c++17 -O2 -fvisibility=hidden -fno-exceptions -Wall -Wextra -Wno-unused-parameter \
               -isysroot $(SDK) -mmacosx-version-min=$(MIN_MACOS) $(ARCHS) -fblocks
LDFLAGS     := -bundle -isysroot $(SDK) -mmacosx-version-min=$(MIN_MACOS) $(ARCHS) \
               -framework CoreAudio -framework CoreFoundation

.PHONY: all driver app service install-driver uninstall-driver install-service uninstall-service install-app run clean

all: driver service app

# Sona Audio Service: the only Sona process allowed to call the Core Audio client HAL.
service: $(SERVICE_OUT)

SERVICE_SRCS := $(SERVICE_DIR)/main.cpp $(SERVICE_DIR)/SonaEngine.cpp $(SERVICE_DIR)/SonaTarget.cpp
SERVICE_HDRS := $(SERVICE_DIR)/SonaEngine.h $(SERVICE_DIR)/SonaTarget.h $(SERVICE_DIR)/SonaHardwareControls.h \
                $(SERVICE_DIR)/SonaRingBuffer.h $(SERVICE_DIR)/SonaResampler.h $(ROOT)/Shared/SonaTransport.h $(ROOT)/Shared/SonaProtocol.h

$(SERVICE_OUT): $(SERVICE_SRCS) $(SERVICE_HDRS)
	@mkdir -p $(SERVICE_DIR)/build
	$(CXX) -std=c++17 -O2 -fno-exceptions -Wall -Wextra -Wno-unused-parameter -fblocks \
		-isysroot $(SDK) -mmacosx-version-min=$(MIN_MACOS) $(ARCHS) \
		-framework CoreAudio -framework CoreFoundation -o $@ $(SERVICE_SRCS)
	codesign --force --sign - --identifier $(SERVICE_LABEL) $@
	@echo "built $@"

# LaunchDaemon: coreaudiod's plug-in host runs in the system domain, so the service must be
# registered there too; a user LaunchAgent is not reachable from it.
install-service: service
	@if [ "$$(id -u)" != "0" ]; then echo "run as: sudo make install-service"; exit 1; fi
	launchctl bootout system/$(SERVICE_LABEL) 2>/dev/null || true
	mkdir -p /Library/PrivilegedHelperTools
	cp $(SERVICE_OUT) $(SERVICE_BIN)
	chown root:wheel $(SERVICE_BIN); chmod 755 $(SERVICE_BIN)
	cp $(SERVICE_DIR)/$(SERVICE_LABEL).plist $(SERVICE_PLIST)
	chown root:wheel $(SERVICE_PLIST); chmod 644 $(SERVICE_PLIST)
	launchctl bootstrap system $(SERVICE_PLIST)
	@echo "installed; check: log show --last 1m --predicate 'eventMessage contains \"SonaService\"'"

uninstall-service:
	@if [ "$$(id -u)" != "0" ]; then echo "run as: sudo make uninstall-service"; exit 1; fi
	launchctl bootout system/$(SERVICE_LABEL) 2>/dev/null || true
	rm -f $(SERVICE_PLIST) $(SERVICE_BIN)

driver: $(DRIVER_OUT)/Contents/MacOS/SonaDriver

# Strict production boundary: the plug-in must not import any Core Audio client HAL symbol.
.PHONY: check-driver-boundary production-driver test-driver-boundary
check-driver-boundary: driver
	python3 $(ROOT)/Tools/check_driver_boundary.py $(DRIVER_OUT)/Contents/MacOS/SonaDriver

production-driver: check-driver-boundary

test-driver-boundary:
	python3 -m unittest discover -s $(ROOT)/Tools/Tests -p 'test_driver_boundary.py'

$(DRIVER_OUT)/Contents/MacOS/SonaDriver: $(DRIVER_SRCS) $(DRIVER_HDRS) $(DRIVER_DIR)/Info.plist
	@mkdir -p $(DRIVER_OUT)/Contents/MacOS
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(DRIVER_SRCS)
	cp $(DRIVER_DIR)/Info.plist $(DRIVER_OUT)/Contents/Info.plist
	codesign --force --sign - --identifier com.sona.driver $(DRIVER_OUT)
	@echo "built $(DRIVER_OUT)"

install-driver: check-driver-boundary
	@if [ "$$(id -u)" != "0" ]; then echo "run as: sudo make install-driver"; exit 1; fi
	rm -rf $(HAL_DIR)/SonaDriver.driver
	cp -R $(DRIVER_OUT) $(HAL_DIR)/SonaDriver.driver
	chown -R root:wheel $(HAL_DIR)/SonaDriver.driver
	killall coreaudiod || true
	@echo "installed; coreaudiod restarted. Check: log show --last 1m --predicate 'subsystem == \"com.sona.driver\"'"

uninstall-driver:
	@if [ "$$(id -u)" != "0" ]; then echo "run as: sudo make uninstall-driver"; exit 1; fi
	rm -rf $(HAL_DIR)/SonaDriver.driver
	killall coreaudiod || true

app: $(APP_DIR)/Resources/AppIcon.icns
	cd $(APP_DIR) && $(SWIFT_BUILD) -c release --arch arm64
	rm -rf $(APP_OUT)
	mkdir -p $(APP_OUT)/Contents/MacOS $(APP_OUT)/Contents/Resources
	cp $(APP_DIR)/.build/apple/Products/Release/Sona $(APP_OUT)/Contents/MacOS/Sona 2>/dev/null || \
	cp $(APP_DIR)/.build/release/Sona $(APP_OUT)/Contents/MacOS/Sona
	cp $(APP_DIR)/Info.plist $(APP_OUT)/Contents/Info.plist
	cp $(APP_DIR)/Resources/AppIcon.icns $(APP_OUT)/Contents/Resources/AppIcon.icns
	codesign --force --sign - --identifier com.sona.app $(APP_OUT)
	@echo "built $(APP_OUT)"

# App icon: pic/SonaIcon.png -> iconset -> icns
$(APP_DIR)/Resources/AppIcon.icns: $(ROOT)/pic/SonaIcon.png
	@mkdir -p $(APP_DIR)/Resources
	rm -rf $(APP_DIR)/build/AppIcon.iconset && mkdir -p $(APP_DIR)/build/AppIcon.iconset
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s $< --out $(APP_DIR)/build/AppIcon.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  d=$$((s*2)); sips -z $$d $$d $< --out $(APP_DIR)/build/AppIcon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns $(APP_DIR)/build/AppIcon.iconset -o $@

install-app: app
	rm -rf /Applications/Sona.app
	cp -R $(APP_OUT) /Applications/Sona.app
	@echo "installed /Applications/Sona.app"

run: app
	open $(APP_OUT)

# Installer package for end users: driver + service + app in one .pkg, with pre/postinstall
# scripts that do what install-service/install-driver do. See Installer/build-installer.sh for
# the Developer ID / notarization variables. Output: build/release/Sona-<version>.pkg.
.PHONY: release
release: check-driver-boundary service app
	sh $(ROOT)/Installer/build-installer.sh

clean:
	rm -rf $(DRIVER_DIR)/build $(SERVICE_DIR)/build $(APP_DIR)/build $(APP_DIR)/.build $(ROOT)/build

# Driver clock slaving against a fake anchor.
.PHONY: test-clock
test-clock:
	@mkdir -p $(DRIVER_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fblocks -framework CoreAudio -framework CoreFoundation \
		$(DRIVER_DIR)/Tests/ClockTests.cpp $(DRIVER_DIR)/SonaTransportClient.cpp -o $(DRIVER_DIR)/build/ClockTests
	$(DRIVER_DIR)/build/ClockTests

.PHONY: test-all
test-all: test test-audio test-app test-transport test-clock test-driver-boundary check-driver-boundary

# Shared-memory transport: layout, SPSC ring, drop/discontinuity, cross-thread stress.
.PHONY: test-transport
test-transport:
	@mkdir -p $(DRIVER_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -O2 -Wall -Wextra -fblocks \
		$(DRIVER_DIR)/Tests/TransportTests.cpp -o $(DRIVER_DIR)/build/TransportTests
	$(DRIVER_DIR)/build/TransportTests

# End-to-end handshake against a running Sona Audio Service (any reachable launchd domain).
# The service only talks to Apple's plug-in host, so the loopback clients are signed with the
# identifier that a test service (SONA_PEER_REQUIREMENT, see test-e2e) accepts instead.
LOOPBACK_TOOLS := $(DRIVER_DIR)/build/TransportLoopback $(DRIVER_DIR)/build/TransportLoopback2 $(DRIVER_DIR)/build/TransportIntruder
.PHONY: loopback-tools test-loopback test-e2e
loopback-tools: $(LOOPBACK_TOOLS) $(ROOT)/Tools/build/sonactl

$(DRIVER_DIR)/build/TransportLoopback: $(DRIVER_DIR)/Tests/TransportLoopback.cpp $(DRIVER_DIR)/SonaTransportClient.cpp $(DRIVER_HDRS)
	@mkdir -p $(DRIVER_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fblocks -framework CoreFoundation \
		$(DRIVER_DIR)/Tests/TransportLoopback.cpp $(DRIVER_DIR)/SonaTransportClient.cpp -o $@
	codesign --force --sign - --identifier com.sona.test.loopback $@

$(DRIVER_DIR)/build/TransportLoopback2: $(DRIVER_DIR)/Tests/TransportLoopback2.cpp $(DRIVER_DIR)/SonaTransportClient.cpp $(DRIVER_HDRS)
	@mkdir -p $(DRIVER_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fblocks -framework CoreFoundation \
		$(DRIVER_DIR)/Tests/TransportLoopback2.cpp $(DRIVER_DIR)/SonaTransportClient.cpp -o $@
	codesign --force --sign - --identifier com.sona.test.loopback $@

# Same program as TransportLoopback under another signing identifier: must never be answered.
$(DRIVER_DIR)/build/TransportIntruder: $(DRIVER_DIR)/build/TransportLoopback
	cp $< $@
	codesign --force --sign - --identifier com.sona.test.intruder $@

$(ROOT)/Tools/build/sonactl: $(ROOT)/Tools/sonactl.swift
	@mkdir -p $(ROOT)/Tools/build
	swiftc -O -o $@ $<

test-loopback: loopback-tools
	$(DRIVER_DIR)/build/TransportLoopback
	$(DRIVER_DIR)/build/TransportLoopback2

# Bootstraps the built service into the caller's launchd domain (no sudo), runs the loopback
# clients plus a rejected intruder against it, and boots it out again.
test-e2e: service loopback-tools
	sh $(SERVICE_DIR)/Tests/e2e-user-domain.sh

# Mock HAL regression tests for the service engine; never change the machine's real output volume.
.PHONY: test
test:
	@mkdir -p $(SERVICE_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -fblocks -Wall -Wextra -Wno-unused-parameter -framework CoreAudio -framework CoreFoundation \
		$(SERVICE_DIR)/Tests/EngineTests.cpp -o $(SERVICE_DIR)/build/EngineTests
	$(SERVICE_DIR)/build/EngineTests

.PHONY: test-audio
test-audio:
	@mkdir -p $(SERVICE_DIR)/build
	$(CXX) -isysroot $(SDK) -std=c++17 -O2 -Wall -Wextra -framework CoreAudio -framework CoreFoundation \
		$(SERVICE_DIR)/Tests/ResamplerTests.cpp $(SERVICE_DIR)/SonaTarget.cpp -o $(SERVICE_DIR)/build/ResamplerTests
	$(SERVICE_DIR)/build/ResamplerTests

.PHONY: test-app
test-app:
	@mkdir -p $(APP_DIR)/build
	swiftc $(APP_DIR)/Sources/Sona/OutputLevelWriter.swift $(APP_DIR)/Tests/OutputLevelWriterTests.swift -o $(APP_DIR)/build/OutputLevelWriterTests
	$(APP_DIR)/build/OutputLevelWriterTests
