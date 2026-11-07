// Runner for the Shared/ tests, called from Core/tests/lh5801_tests.cpp's
// main() like every other suite. Build & run: tools/run_tests.sh.

int run_shared_geometry_tests();
int run_shared_host_key_map_tests();
int run_shared_settings_tests();
int run_shared_machine_session_tests();
int run_shared_pacing_tests();
int run_shared_bay_tests();
int run_shared_app_session_tests();
int run_shared_preset_catalog_tests();

int run_shared_tests() {
    int failures = 0;
    failures += run_shared_geometry_tests();
    failures += run_shared_host_key_map_tests();
    failures += run_shared_settings_tests();
    failures += run_shared_machine_session_tests();
    failures += run_shared_pacing_tests();
    failures += run_shared_bay_tests();
    failures += run_shared_app_session_tests();
    failures += run_shared_preset_catalog_tests();
    return failures;
}
