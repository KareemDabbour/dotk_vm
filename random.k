
fn nextInt(low, high){
    return int(randFloat(low, high));
}
fn randFloat(low, high){
    return rand() * high + low;
}
fn randBool(){
    return rand() >= 0.5;
}
