fn with(closable, f){
    var ret = f(closable)
    try{
        closable.close()
    }catch(e){
        print(e.message)
    }
    return ret
}

fn min(a, b){
    if(a < b){
        return a
    }
    return b
}

fn max(a, b){
    if(a > b){
        return a
    }
    return b
}

fn abs(a){
    if(a < 0){
        return -a
    }
    return a
}

fn __hierarchy__(obj){
    var oType = type(obj) 
    if (oType != "class"){
        write(1,f"type: {oType} is not a class\n")
        return {};
    }

    var h = []
    var r = obj
    while(r){
        h.append(r)
        r = r.superClass
    }

    return h[:@-1]
}

class Ansi{
    BOLD = '\e[1m'
    DIM = '\e[2m'
    UNDERLINED = '\e[4m'
    BLINK = '\e[5m'
    REVERSE = '\e[7m'
    HIDDEN = '\e[8m'
    BLACK = '\e[0;30m'
    RED = '\e[0;31m'
    GREEN = '\e[0;32m'
    YELLOW = '\e[0;33m'
    BLUE = '\e[0;34m'
    PURPLE = '\e[0;35m'
    CYAN = '\e[0;36m'
    WHITE = '\e[0;37m'
    RESET = '\e[0m'

    ALL_COLORS = {
        Ansi.BLACK, 
        Ansi.RED, 
        Ansi.GREEN, 
        Ansi.BLUE, 
        Ansi.PURPLE, 
        Ansi.CYAN, 
        Ansi.YELLOW, 
        Ansi.WHITE,
    }
}
