CXX ?= c++
AR ?= ar
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wpedantic
BUILD ?= build
LDLIBS += -pthread

.PHONY: all check sanitize clean
all: $(BUILD)/libmosp_evaluator.a $(BUILD)/mosp-evaluate $(BUILD)/pt-example $(BUILD)/adaptive-pt-example

$(BUILD):
	mkdir -p $@

$(BUILD)/evaluator.o: src/evaluator.cpp include/mosp/evaluator.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/adaptive.o: src/adaptive.cpp include/mosp/adaptive.hpp include/mosp/evaluator.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/libmosp_evaluator.a: $(BUILD)/evaluator.o $(BUILD)/adaptive.o
	$(AR) rcs $@ $^

$(BUILD)/mosp-evaluate: tools/evaluate.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/pt-example: examples/pt_adapter.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/adaptive-pt-example: examples/adaptive_pt.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-evaluator: tests/test_evaluator.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/benchmark: benchmarks/benchmark.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/auto-benchmark: benchmarks/auto_benchmark.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-allocations: tests/test_allocations.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/test-adaptive: tests/test_adaptive.cpp $(BUILD)/libmosp_evaluator.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(BUILD)/libmosp_evaluator.a $(LDFLAGS) $(LDLIBS) -o $@

check: all $(BUILD)/test-evaluator $(BUILD)/test-allocations $(BUILD)/test-adaptive
	$(BUILD)/test-evaluator
	$(BUILD)/test-allocations
	$(BUILD)/test-adaptive
	$(BUILD)/pt-example
	$(BUILD)/adaptive-pt-example
	python3 tests/test_cli.py $(BUILD)/mosp-evaluate

sanitize:
	$(MAKE) check BUILD=build-sanitize CXXFLAGS="-O1 -g -std=c++17 -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

# Removes only this project's default generated build directories.
clean:
	python3 -c "import shutil; shutil.rmtree('build', ignore_errors=True); shutil.rmtree('build-sanitize', ignore_errors=True)"
