module textx {
    fn greet(name) {
        return _privateFunc() + name;
    }

    fn _privateFunc() {
        return "hi ";
    }

    export greet;
}
