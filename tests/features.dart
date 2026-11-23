class Point {
    int x;
    int y;
    Point(this.x, this.y);
    int sum() => this.x + this.y;
}

void main() {
    Point p = Point(10, 20);
    print(p.sum());

    int total = 0;
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            continue;
        } else if (i == 7) {
            break;
        }
        total += i;
    }
    print(total);

    List<int> nums = [10, 20, 30, 40, 50];
    print(nums[1:4]);

    string s = "chibidart";
    print(s[0:5]);
}

main();
