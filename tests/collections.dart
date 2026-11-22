void main() {
    List<int> numbers = [1, 2, 3];
    numbers.push(4);
    numbers[0] = 99;
    print(numbers);
    print(numbers.length);

    Map<string, int> scores = {"alice": 10, "bob": 20};
    scores["charlie"] = 30;
    print(scores);
    print(scores.length);
    print(scores.has("alice"));
    scores.remove("alice");
    print(scores.has("alice"));
}

main();
