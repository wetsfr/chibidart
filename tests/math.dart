double circle_area(double radius) {
    double pi = 3.1415926535;
    return pi * radius * radius;
}

void main() {
    float r = 2.5;
    double area = circle_area(r);
    print(area);
}

main();
