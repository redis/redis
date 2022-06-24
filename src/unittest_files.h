extern int sha1Test(int argc, char **argv, int flags);
extern int sdsTest(int argc, char **argv, int flags);
extern int ziplistTest(int argc, char **argv, int flags);
extern int dictTest(int argc, char **argv, int flags);
extern int endianconvTest(int argc, char *argv[], int flags);
extern int listpackTest(int argc, char *argv[], int flags);
extern int zipmapTest(int argc, char *argv[], int flags);
extern int utilTest(int argc, char **argv, int flags);
extern int quicklistTest(int argc, char *argv[], int flags);
extern int intsetTest(int argc, char **argv, int flags);
extern int crc64Test(int argc, char *argv[], int flags);

typedef int redisTestProc(int argc, char **argv, int flags);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {
    {"sha1", sha1Test},
    {"sds", sdsTest},
    {"ziplist", ziplistTest},
    {"dict", dictTest},
    {"endianconv", endianconvTest},
    {"listpack", listpackTest},
    {"zipmap", zipmapTest},
    {"util", utilTest},
    {"quicklist", quicklistTest},
    {"intset", intsetTest},
    {"crc64", crc64Test},
};
