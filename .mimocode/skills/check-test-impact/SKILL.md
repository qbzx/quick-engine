---
name: check-test-impact
description: "After making code changes, analyze which unit tests are affected, run them, and fix failures. Use when the user asks to check/update/supplement tests after a code modification."
---

# Unit Test Impact Analysis

Systematically find and fix unit tests affected by recent code changes.

## When To Use

- User says: "检查相关单元测试是否需要更新/修改/调整" or "补齐单元测试"
- User selects changed code and asks about test impact
- After editing a function/class/module, verifying tests still pass
- User asks to run a specific test file

## Procedure

### Step 1 — Identify What Changed

```
1. Check `git diff` or `git diff --cached` to see modified files
2. If user selected specific lines, focus on those
3. Extract: changed file paths, modified function/class/method names
```

For each changed file, note:
- **Module path** (e.g., `lms.api.org`, `lms.learning_activity.services`)
- **Function/class names** that were added, modified, or removed
- **Import changes** that may affect other modules

### Step 2 — Locate Related Test Files

Search for tests using multiple strategies in parallel:

```
# By filename convention
find <project>/pytests -name "*<module_keyword>*" -type f       # Python
find <project>/tests -name "*<module_keyword>*" -type f         # JS/TS

# By content — grep for imports or references to changed code
grep -rn "<changed_function>" <test_dir>/ --include="*.py" -l
grep -rn "<changed_class>" <test_dir>/ --include="*.py" -l
grep -rn "<module_path>" <test_dir>/ --include="*.py" -l

# By fixture/factory usage (if entities or schemas changed)
grep -rn "<model_name>\|<factory_name>" <test_dir>/ --include="*.py" -l
```

Record the candidate test files and the specific test methods that reference changed code.

### Step 3 — Read and Analyze

For each candidate test file:
1. Read the relevant test methods
2. Determine if the test **directly exercises** the changed code
3. Classify each test:
   - **Affected** — tests that call the changed function/endpoint directly
   - **Potentially affected** — tests that use the same module but indirectly
   - **Not affected** — tests in the same file but unrelated

### Step 4 — Run Affected Tests

**Python (pytest):**
```bash
# Run specific test file
ENV=test py.test <test_file> -v 2>&1 | tail -60

# Run specific test method
ENV=test py.test <test_file>::<TestClass>::<test_method> -v 2>&1 | tail -60

# Run with migration if DB schema changed
ENV=test inv db.migrate 2>&1 | tail -20
ENV=test py.test <test_file> -v 2>&1 | tail -60
```

**JavaScript/TypeScript (jest):**
```bash
npx jest <test_files> --verbose 2>&1 | tail -60
```

### Step 5 — Fix Failures

If tests fail:

1. **Read the full error output** — identify the exact assertion or import failure
2. **Read the test file** around the failure point
3. **Diagnose**: Is it a real bug in the code, or does the test need updating?
4. **Edit the test** to match the new behavior
5. **Re-run** to confirm the fix

Common fix patterns:
- Updated return value shape → update expected dict in assertion
- Added required parameter → update test fixture/factory call
- Removed function → delete or skip the test
- Changed behavior → update expected behavior in assertion
- New enum value → add test case for new value

### Step 6 — Report

Summarize what was found and done:
- Which test files were checked
- Which tests were affected and how
- What fixes were applied
- Which tests now pass

## Anti-Patterns

- **Don't run all tests** — only the affected subset, to keep feedback fast
- **Don't skip reading the test** — always understand what the test was checking before fixing it
- **Don't guess at failures** — run the test first, read the output, then diagnose
- **Don't ignore migration issues** — if DB schema changed, ensure test DB is migrated first

## Project-Specific Notes

This project uses:
- Python: `ENV=test py.test <path> -v` for pytest
- JavaScript: `npx jest <path> --verbose` for Jest
- Test directories: `pytests/` (Python), `tests/unit/` (JS/TS)
