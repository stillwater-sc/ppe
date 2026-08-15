# PPE tests -- assertions about the model layer, not about the hardware.
#
# What belongs here: anything with a right answer that does not depend on the
# machine it runs on. The peak model is the archetype -- given a stated ISA and
# unit count, its ops/cycle table is arithmetic, and it must reproduce the
# hand-written constants it generalizes.
#
# What does NOT belong here: measurements. A test that asserts a bandwidth or a
# latency is asserting a property of the runner, and on shared CI hardware that
# is a flake generator rather than a check. Those live in applications/ and are
# never gated on.
#
# Every "*.cpp" here becomes target "test_<filename_stem>" and is registered
# with CTest to RUN (unlike the executables elsewhere, which are only smoke
# tested with --help).

file(GLOB PPE_TEST_SRC CONFIGURE_DEPENDS "*.cpp")

compile_all("true" "test" "Tests" "PPE::ppe" ${PPE_TEST_SRC})
