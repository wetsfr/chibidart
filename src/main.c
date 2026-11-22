#include "../include/chibidart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int native_sq(cd_State* cd) {
    int64_t n = cd_toint(cd, 1);
    cd_pushint(cd, n * n);
    return 1;
}

static void run_tests(void) {
    cd_State* cd = cd_open();
    assert(cd != NULL);

    int r = cd_dostring(cd, "int a = 10; int b = 20; print(a + b);");
    assert(r == CD_OK);

    r = cd_dostring(cd, "string s1 = \"hello \"; string s2 = \"world\"; print(s1 + s2);");
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "double x = 3.14;\n"
        "float y = 2.0;\n"
        "double z = x * y + 1.5;\n"
        "print(z);\n"
    );
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "int fib(int n) {\n"
        "    if (n <= 1) return n;\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "print(fib(10));\n"
    );
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "int sum = 0;\n"
        "for (int i = 0; i <= 10; i = i + 1) {\n"
        "    sum += i;\n"
        "}\n"
        "print(sum);\n"
    );
    assert(r == CD_OK);

    cd_register(cd, "host_square", native_sq);
    r = cd_dostring(cd,
        "int v = host_square(7);\n"
        "print(v);\n"
    );
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "List<int> lst = [1, 2, 3];\n"
        "lst.push(4);\n"
        "lst[0] = 10;\n"
        "print(lst);\n"
    );
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "Map<string, int> m = {\"a\": 100, \"b\": 200};\n"
        "m[\"c\"] = 300;\n"
        "print(m);\n"
    );
    assert(r == CD_OK);

    r = cd_dostring(cd,
        "class Point {\n"
        "    int x;\n"
        "    int y;\n"
        "    void init(int x, int y) {\n"
        "        this.x = x;\n"
        "        this.y = y;\n"
        "    }\n"
        "    int distSq() => this.x * this.x + this.y * this.y;\n"
        "}\n"
        "Point p = Point(3, 4);\n"
        "print(p.distSq());\n"
    );
    assert(r == CD_OK);

    cd_close(cd);
    printf("all tests passed\n");
}

int main(int argc, char** argv) {
    if (argc == 1) {
        printf("usage: chibidart [script.dart | -e \"code\" | --test]\n");
        return 0;
    }

    if (strcmp(argv[1], "--test") == 0) {
        run_tests();
        return 0;
    }

    cd_State* cd = cd_open();
    if (!cd) return 1;

    int result = 0;
    if (strcmp(argv[1], "-e") == 0 && argc > 2) {
        result = cd_dostring(cd, argv[2]);
    } else {
        result = cd_dofile(cd, argv[1]);
    }

    cd_close(cd);
    return result == CD_OK ? 0 : 1;
}
