// xeno_test_runner.c
// Simple test runner for XenoScript using xenoc.exe and xenovm.exe
// Windows-oriented (uses _popen). Adapt as needed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <io.h>

#define MAX_PATH_LEN 1024
#define MAX_BUF      65536

typedef struct TestResult {
    const char* name;
    int passed;
} TestResult;

static int read_file(const char* path, char* buffer, size_t buffer_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buffer, 1, buffer_size - 1, f);
    fclose(f);
    buffer[n] = '\0';
    return 1;
}

static int file_exists(const char* path) {
    return _access(path, 0) == 0;
}

static int run_command_capture(const char* cmd, char* out, size_t out_size) {
    FILE* pipe = _popen(cmd, "r");
    if (!pipe) return 0;

    size_t total = 0;
    while (!feof(pipe) && total < out_size - 1) {
        size_t n = fread(out + total, 1, out_size - 1 - total, pipe);
        if (n == 0) break;
        total += n;
    }
    out[total] = '\0';

    int rc = _pclose(pipe);
    return rc;
}

static int compare_text(const char* expected, const char* actual) {
    // Trim trailing whitespace for robustness
    size_t elen = strlen(expected);
    size_t alen = strlen(actual);
    while (elen > 0 && (expected[elen - 1] == '\n' || expected[elen - 1] == '\r' || expected[elen - 1] == ' ' || expected[elen - 1] == '\t')) elen--;
    while (alen > 0 && (actual[alen - 1] == '\n' || actual[alen - 1] == '\r' || actual[alen - 1] == ' ' || actual[alen - 1] == '\t')) alen--;

    if (elen != alen) return 0;
    return strncmp(expected, actual, elen) == 0;
}

int main(void) {
    char cwd[MAX_PATH_LEN];
    _getcwd(cwd, sizeof(cwd));

    char search_pattern[MAX_PATH_LEN];
    snprintf(search_pattern, sizeof(search_pattern), "%s\\test\\*.xeno", cwd);

    struct _finddata_t fdata;
    intptr_t handle = _findfirst(search_pattern, &fdata);
    if (handle == -1) {
        printf("No test files found in ./test\n");
        return 1;
    }

    TestResult results[512];
    int result_count = 0;
    int passed_count = 0;
    int failed_count = 0;

    do {
        char test_name[MAX_PATH_LEN];
        snprintf(test_name, sizeof(test_name), "%s", fdata.name);

        // Strip extension
        char base_name[MAX_PATH_LEN];
        snprintf(base_name, sizeof(base_name), "%s", fdata.name);
        char* dot = strrchr(base_name, '.');
        if (dot) *dot = '\0';

        char xeno_path[MAX_PATH_LEN];
        snprintf(xeno_path, sizeof(xeno_path), "%s\\test\\%s", cwd, fdata.name);

        char bytecode_path[MAX_PATH_LEN];
        snprintf(bytecode_path, sizeof(bytecode_path), "%s\\test\\%s.xbc", cwd, base_name);

        char expected_out_path[MAX_PATH_LEN];
        snprintf(expected_out_path, sizeof(expected_out_path), "%s\\test\\%s.out", cwd, base_name);

        char expected_err_path[MAX_PATH_LEN];
        snprintf(expected_err_path, sizeof(expected_err_path), "%s\\test\\%s.err", cwd, base_name);

        printf("=== Running test: %s ===\n", test_name);

        char cmd[MAX_PATH_LEN * 2];
        char compile_output[MAX_BUF] = {0};
        char run_output[MAX_BUF] = {0};

        // 1) Compile with xenoc.exe (capture stdout/stderr merged)
        snprintf(cmd, sizeof(cmd),
                 "\"%s\\bin\\xenoc.exe\" \"%s\" -o \"%s\" 2>&1",
                 cwd, xeno_path, bytecode_path);

        int compile_rc = run_command_capture(cmd, compile_output, sizeof(compile_output));

        // 2) Run with xenovm.exe if compile succeeded (or even if not, depending on your semantics)
        int run_rc = 0;
        if (file_exists(bytecode_path)) {
            snprintf(cmd, sizeof(cmd),
                     "\"%s\\bin\\xenovm.exe\" \"%s\" 2>&1",
                     cwd, bytecode_path);
            run_rc = run_command_capture(cmd, run_output, sizeof(run_output));
        }

        // 3) Load expected outputs (if present)
        char expected_out[MAX_BUF] = {0};
        char expected_err[MAX_BUF] = {0};
        int has_expected_out = file_exists(expected_out_path) && read_file(expected_out_path, expected_out, sizeof(expected_out));
        int has_expected_err = file_exists(expected_err_path) && read_file(expected_err_path, expected_err, sizeof(expected_err));

        int passed = 1;

        // If .out exists, compare VM stdout to it
        if (has_expected_out) {
            if (!compare_text(expected_out, run_output)) {
                printf("  [FAIL] stdout mismatch\n");
                printf("  Expected:\n%s\n", expected_out);
                printf("  Actual:\n%s\n", run_output);
                passed = 0;
            }
        }

        // If .err exists, compare combined compile+run stderr to it
        if (has_expected_err) {
            char combined_err[MAX_BUF * 2] = {0};
            snprintf(combined_err, sizeof(combined_err), "%s%s", compile_output, run_output);

            if (!compare_text(expected_err, combined_err)) {
                printf("  [FAIL] stderr mismatch\n");
                printf("  Expected:\n%s\n", expected_err);
                printf("  Actual:\n%s\n", combined_err);
                passed = 0;
            }
        }

        // If neither .out nor .err exist, just require successful compile+run
        if (!has_expected_out && !has_expected_err) {
            if (compile_rc != 0 || run_rc != 0) {
                printf("  [FAIL] non-zero exit code (compile=%d, run=%d)\n", compile_rc, run_rc);
                printf("  Compile output:\n%s\n", compile_output);
                printf("  Run output:\n%s\n", run_output);
                passed = 0;
            }
        }

        results[result_count].name = _strdup(test_name);
        results[result_count].passed = passed;
        result_count++;

        if (passed) {
            printf("  [PASS]\n");
            passed_count++;
        } else {
            printf("  [FAIL]\n");
            failed_count++;
        }

        // Optionally delete bytecode after test
        remove(bytecode_path);

    } while (_findnext(handle, &fdata) == 0);

    _findclose(handle);

    printf("\n=== Test Summary ===\n");
    printf("Total:  %d\n", result_count);
    printf("Passed: %d\n", passed_count);
    printf("Failed: %d\n", failed_count);

    if (failed_count > 0) {
        printf("\nFailed tests:\n");
        for (int i = 0; i < result_count; ++i) {
            if (!results[i].passed) {
                printf("  %s\n", results[i].name);
            }
        }
    }

    return failed_count == 0 ? 0 : 1;
}
