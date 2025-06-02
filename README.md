# Cox
Cox is an event-driven architecture designed for high performance.

## Code Style
The project strictly follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

## Unit Tests (UT)
The project includes unit tests to ensure code quality and correctness.

### How to Run Test Cases
Here are some common commands for running test cases using Google Test:

#### Run All Test Cases
To run all test cases:
```bash
./build/bin/event_loop_test
```

#### Run an Entire Test Suite
To run all tests in a specific test suite:
```bash
./build/bin/event_loop_test --gtest_filter=TestSuiteName.*
```

#### Run a Specific Test
To run a single test:
```bash
./build/bin/event_loop_test --gtest_filter=TestSuiteName.TestName
```

#### Run Multiple Tests
To run multiple specific tests:
```bash
./build/bin/event_loop_test --gtest_filter=TestSuiteName.TestName1:TestSuiteName.TestName2
```

#### Exclude a Test
To exclude a specific test:
```bash
./build/bin/event_loop_test --gtest_filter=-TestSuiteName.TestName
```

#### List All Available Tests
To list all available tests in the test binary:
```bash
./build/bin/event_loop_test --gtest_list_tests
```