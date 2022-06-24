import os
import re

UNIT_DIR = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + '/../src/unit')
TEST_FILE = UNIT_DIR + '/test_files.h'
TEST_PROTOTYPE = '(int ([a-zA-Z].*)Test\(.*\))'

if __name__ == '__main__':
    with open(TEST_FILE, 'w') as output:
        # Find each test file and collect the test names.
        tests = []
        for root, dirs, files in os.walk(UNIT_DIR):
            for file in files:
                file_path = UNIT_DIR + '/' + file
                if not file.endswith('.c') or file == 'test_main.c':
                    continue
                # Read contents of file
                with open(file_path, 'r') as f:
                    for line in f:
                        match = re.match(TEST_PROTOTYPE, line)
                        if match:
                            function = match.group(1)
                            test_name = match.group(2)
                            tests.append((test_name, function))
        for test in tests:
            output.write('extern {};\n'.format(test[1]))

        output.write("""
typedef int redisTestProc(int argc, char **argv, int flags);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {
""")
        for test in tests:
            output.write('    {{"{0}", {0}Test}},\n'.format(test[0], test[1]))
        output.write('};\n')