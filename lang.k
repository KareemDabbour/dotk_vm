class Closable{
    init(){
        print_error("Cannot instatiate Closable type. Must be an implementation!")
    }

    close(){
        print_error("Not Implemented!")
    }
}

fn with(closable, f){
    if (!instanceof(closable ,Closable)) print_error("Cannot use the 'with(Closable, fn)' function with a non Closable type!")
    var ret = f(closable)
    closable.close()
    return ret
}