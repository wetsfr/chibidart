class Hero {
    string name;
    int hp;

    void init(string name, int hp) {
        this.name = name;
        this.hp = hp;
    }

    void takeDamage(int dmg) {
        this.hp = this.hp - dmg;
    }

    bool isAlive() => this.hp > 0;
}

void main() {
    Hero h = Hero("Knight", 100);
    print(h.name);
    print(h.hp);
    h.takeDamage(35);
    print(h.hp);
    print(h.isAlive());
}

main();
