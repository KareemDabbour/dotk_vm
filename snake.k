import "stdlib.k"
import "file.k"

# var rows = 30;
var size = 30;
var line = " " + ("".join([size]{"---"})) + " ";
var highscoresFile = "snake_HS.csv";

class Field{
    init(size){
        this.size = size;
        this.icons = {" . "," * ", " # "," & "};
        this.board = this.create_board(size);
        this.snakeCoords = ArrayList();
        this.entityPos = {-1, -1};
        this.addEntity();
    }
    create_board(size){
        var ret = [size]{0};
        for (var i = 0; i < size; i += 1){
            ret[i] = [size]{0};
        }
        return ret;
    }
    addEntity(){
        while(true){
            var x = randInt(0, this.size-1);
            var y = randInt(0, this.size-1);
            if (!this.snakeCoords.contains({x, y})){

                this.board[y][x] = 3;
                this.entityPos[0] = y;
                this.entityPos[1] = x;
                break;
            }
        }
    }
    
    getEntityPos(){
        return this.entityPos;
    }

    didSnakeEat(){
        var head = this.snakeCoords.getLast();
        var entity = this.getEntityPos();
        return head == entity;
    }

    clearBoard(){
        for (var y = 0; y < this.size; y += 1){
            for (var x = 0; x < this.size; x += 1){
                if(this.board[y][x] == 1 || this.board[y][x] == 2)
                    this.board[y][x] = 0;
            }
        }
    }

    toStr(){
        this.clearBoard();
        for (var i = 0; i < len(this.snakeCoords); i += 1){
            var coord = this.snakeCoords.get(i);
            this.board[coord[0]][coord[1]] = 1;
        }
        var head = this.snakeCoords.getLast();
        if(head)
            this.board[head[0]][head[1]] = 2;

        var sb = StringBuilder(line).append("\n");
        for (var y = 0; y < this.size; y += 1){
            sb.append("|");
            for (var x = 0; x < this.size; x += 1){
                sb.append(this.icons[this.board[y][x]]);
            }
            sb.append("|\n");
        }
        sb.append(line);
        return sb.toStr();
    }
}

class Directions{
    UP = 0;
    DOWN = 1;
    LEFT = 2;
    RIGHT = 3;
}

class Snake {
    init(name){
        this.name = name;
        this.direction = Directions.RIGHT;
        this.coords = ArrayList.of({{0, 0}, {0, 1}, {0, 2}, {0, 3}});
        this.field = null;
        this.score = 0;
    }

    setDirection(newDirection){
        if (newDirection == Directions.UP && this.direction != Directions.DOWN){
            
            this.direction = newDirection;
        }else if (newDirection == Directions.DOWN && this.direction != Directions.UP){
            
            this.direction = newDirection;
        }else if (newDirection == Directions.LEFT && this.direction != Directions.RIGHT){
            
            this.direction = newDirection;
        }else if (newDirection == Directions.RIGHT && this.direction != Directions.LEFT){
            
            this.direction = newDirection;
        }
    }

    levelUp(){
        var a = this.coords.get(0);
        var b = this.coords.get(1);
        var tail = a[:];
        if(a[0] < b[0]){
            tail[0] = tail[0] - 1;
        }else if(a[0] > b[0]){
            tail[0] =  tail[0] + 1;
        }else if(a[1] < b[1]){
            tail[1] = tail[1] - 1;
        }else if(a[1] > b[1]){
            tail[1] =  tail[1] + 1;
        }

        tail = this.wrap(tail);
        this.coords.addAtIndex(0, tail);
        this.score = this.score + 1;

    }

    isAlive(){
        var head = this.coords.getLast();
        for (var i = 0; i < len(this.coords) - 1; i += 1){
            if (head == this.coords.get(i)){
                return false;
            }
        }
        return true;
    }

    move(){
        var newHead = this.coords.getLast()[:];
        switch(this.direction){
            case Directions.UP{
                newHead[0] = newHead[0] - 1;
            }
            case Directions.DOWN{
                newHead[0] = newHead[0] + 1;
            }
            case Directions.LEFT{
                newHead[1] = newHead[1] - 1;
            }
            case Directions.RIGHT{
                newHead[1] = newHead[1] + 1;
            }
        }

        newHead = this.wrap(newHead);
        this.coords.add(newHead);
        # print "SNAKE: " + this.coords  + " SIZE: " + len(this.coords);
        this.coords.remove(0);
        this.field.snakeCoords = this.coords;

        if(!this.isAlive()){
            return true;
        }
        if(this.field.didSnakeEat()){
            this.levelUp();
            this.field.addEntity();
        }
        return false;
    }

    wrap(coord){
        coord[0] = (coord[0]) % size;
        coord[1] = (coord[1]) % size;
        if(coord[0] < 0){
            coord[0] = size - 1;
        }
        if(coord[1] < 0){
            coord[1] = size - 1;
        }
        return coord;
    }
    setField(field){
        this.field = field;
    }
}

fn getKeyInput(){
    var c = getc();
    switch(c){
        case 27{
            getc();
            switch(getc()){
                case 65 {
                    return 0; # UP
                }
                case 66 {
                    return 1; # DOWN
                }
                case 67 {
                    return 3; #RIGHT
                }
                case 68 {
                    return 2; #LEFT
                }
            }
        }

        default{
            return c;
        }
    }
}

fn printScoreAndField(s, f){
    print "NAME: " + s.name;
    print "SCORE: " + s.score;
    print(f);
}




print("Welcome to Snake.k!");
var name = input("What is your name?\n");
clear();
# var name = "SNAKE";
var f = Field(size);
var s = Snake(name);
s.setField(f);

fn run (){
    while(true){
        var died = s.move();
        printScoreAndField(s, f);
        print("<p> to pause -- <q> to quit");
        if(kbhit()){
            var d = getKeyInput();
            if(d == 112){
                clear();
                printScoreAndField(s, f);
                print("<p> to un-pause -- <q> to quit");
                while(true){
                    if(kbhit()){
                        var c = getKeyInput();
                        if(c == 112){
                            break;  
                        }else if(c == 113){
                            return "QUIT";
                        }
                    }
                    sleep(0.016);
                }
            }else if(d == 113){
                return "QUIT";
            }else{
                s.setDirection(d);
            }
        }
        if(died) return "DIED";
        clear();
        # a sleep call that scales with the size of the field to make the game speed more consistent

        sleep(0.14 - (0.1 * (size / 100)));

        # sleep(0.15);
    }
}

var g = run();

clear();
printScoreAndField(s, f);

if (g == "QUIT"){
    print "GOODBYE!";
}else if (g == "DIED"){
    print "YOU DIED";
}

print "\nYOU SCORED " + s.score + " POINTS!";

var hs = File(highscoresFile, F_RW | F_CR, null);

var scores = hs.readAll().trim()

var lines = scores.split("\n");

var m = Map()

foreach(ArrayList.of(lines), fn(it){
    var line = it.trim();
    var parts = line.split(",");
    if (len(parts) == 2) {
        m.put(parts[0], int(parts[1]));
    }
});
var oldScore = m.getOrDefault(s.name, 0)
if (oldScore < s.score) {
    m.put(s.name, s.score);
    print "CONGRAULATIONS "+ s.name +"! YOU GOT NEW HIGH SCORE!";
    print "OLD: " + oldScore + " NEW: " + s.score;
}

m.sortByValue(SortOrder.DESC);
print line;
print "HIGH SCORES: (TOP 3)";
var iterator = m.entrySet.iterator()

while(iterator.hasNext() && iterator.i <= 1){
    var entry = iterator.next();
    print entry.key + ": " + entry.value;
}

print line;

hs.resetCursor();



foreach(m.entrySet, fn(it){
    hs.write(it.key + "," + it.value + "\n")
})
print "SCORE SAVED!";



hs.close();