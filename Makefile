# Makefile for AMD RDNA 3 (Radeon 780M) macOS Driver Project

CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
INCLUDES = -Isrc -Isrc/amd -Isrc/uapi -Isrc/winsys

TEST_BIN_DIR = build/tests
TESTS = $(TEST_BIN_DIR)/test_rdna3_discovery_uma \
        $(TEST_BIN_DIR)/test_rdna3_ip_dcn \
        $(TEST_BIN_DIR)/test_rdna3_ucode \
        $(TEST_BIN_DIR)/test_rdna3_chip_profile \
        $(TEST_BIN_DIR)/test_rdna3_fw_plan \
        $(TEST_BIN_DIR)/test_phx_dcn_pipe \
        $(TEST_BIN_DIR)/test_phx_dcn_i2c \
        $(TEST_BIN_DIR)/test_phx_dcn_timing \
        $(TEST_BIN_DIR)/test_phx_dmub_layout \
        $(TEST_BIN_DIR)/test_phx_dmub_vbios_cmd \
        $(TEST_BIN_DIR)/test_r3n_abi

.PHONY: all test clean kext fixtures

all: test

$(TEST_BIN_DIR):
	mkdir -p $(TEST_BIN_DIR)

$(TEST_BIN_DIR)/test_rdna3_discovery_uma: src/amd/rdna3_ip_discovery.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_discovery_uma.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ip_dcn: src/amd/rdna3_ip_blocks.cpp src/amd/rdna3_dcn314_scanout.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_ip_dcn.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ucode: src/amd/rdna3_ucode_extract.cpp tests/test_rdna3_ucode.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_chip_profile: src/amd/rdna3_ip_discovery.cpp src/amd/rdna3_chip_profile.cpp tests/test_rdna3_chip_profile.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_fw_plan: src/amd/rdna3_ucode_extract.cpp src/amd/rdna3_fw_plan.cpp tests/test_rdna3_fw_plan.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_phx_dcn_pipe: tests/test_phx_dcn_pipe.cpp src/phxaccel/accel/phx_dcn_pipe.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/phxaccel/accel tests/test_phx_dcn_pipe.cpp -o $@

$(TEST_BIN_DIR)/test_phx_dcn_i2c: tests/test_phx_dcn_i2c.cpp src/amd/phx_dcn_i2c.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/amd tests/test_phx_dcn_i2c.cpp -o $@

$(TEST_BIN_DIR)/test_phx_dcn_timing: tests/test_phx_dcn_timing.cpp src/amd/phx_dcn_timing.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/amd tests/test_phx_dcn_timing.cpp -o $@

$(TEST_BIN_DIR)/test_phx_dmub_layout: tests/test_phx_dmub_layout.cpp src/amd/phx_dmub_layout.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/amd tests/test_phx_dmub_layout.cpp -o $@

$(TEST_BIN_DIR)/test_phx_dmub_vbios_cmd: tests/test_phx_dmub_vbios_cmd.cpp src/amd/phx_dmub_vbios_cmd.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/amd tests/test_phx_dmub_vbios_cmd.cpp -o $@

$(TEST_BIN_DIR)/test_r3n_abi: src/uapi/r3n_user_client.cpp src/winsys/radv_r3n_winsys.cpp tests/test_r3n_abi.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

test: $(TESTS)
	@echo "=== Running RDNA 3 Driver Unit Tests ==="
	@for t in $(TESTS); do \
		echo "Running $$t..."; \
		$$t || exit 1; \
	done
	@echo "All unit tests passed successfully!"

fixtures:
	bash tools/fetch_test_firmware.sh tests/fixtures/phoenix

kext:
	@echo "Building PhxBringup.kext..."
	python3 tools/build_kext.py

clean:
	rm -rf build/
