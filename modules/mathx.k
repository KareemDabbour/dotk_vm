const PI = 3.14;
const e = 2.71828;
const INF = 1.0/0.0;
const NEG_INF = -1.0/0.0;

const floor = fn(num) {
    if (num < 0){
        return num // 1 - 1;
    }
    return num // 1;
}

const ceil = fn(num) {
    if (num < 0 || num == num // 1){
        return num // 1;
    }
    return num//1 + 1;
}

const sqrt = fn(num) => num >= 0 ? num ** 0.5 : 0/0;

const gcd = fn(a, b){
    if (a == 0){
        return b;
    }
    return gcd(b % a, a);
}

const lcm = fn(a, b) => (a * b) // gcd(a, b);

const lcmAll = fn(arr) {
    var ret = 1;
    for (var i = 0; i < len(arr); i+=1){
        ret = lcm(ret, arr[i]);
    }
    return ret;
}

const abs = fn(num) => num < 0 ? -num : num

export PI, e, INF, NEG_INF, floor, ceil, sqrt, gcd, lcm, lcmAll, abs;
