~~import "stdlib.k";
var BOARD_SIZE = 50;
var EFF_BOARD_SIZE = BOARD_SIZE + 2;

var ICONS = {" . ", " # ",};

var PIECES = {

    {{1}, {1}, {1}, {1}},

    {{1, 0},
     {1, 0},
     {1, 1}},   
    
    {{0, 1},
     {0, 1},
     {1, 1}},

    {{0, 1},
     {1, 1},
     {0, 1}},

    {{0, 1},
     {1, 1},
     {1, 0}},

    {{1, 0},
     {1, 1},
     {0, 1}},

    {{1, 1},
     {1, 1}}

};

var MOVE_LEFT = "a";
var MOVE_RIGHT = "d";
var ROTATE_ANTICLOCKWISE = "w";
var ROTATE_CLOCKWISE = "s";
var NO_MOVE = "e";
var QUIT_GAME = "q";

fn printBoard(board, score, currPiece, nextPiece, piecePos, errMsg){

    var boardCopy = copyBoard(board);
    var currPieceX = len(currPiece);
    var currPieceY = len(currPiece[0]);
    for(var x = 0; x<currPieceX; x+=1){
        for(var y = 0; y<currPieceY; y+=1){
            boardCopy[piecePos[0] + x][piecePos[1] + y] = currPiece[x][y] | boardCopy[piecePos[0] + x][piecePos[1] + y];
        }
    }

    # print boardCopy
    var sb = StringBuilder("SCORE: ").append(score).append("\n");
    for (var row = 0; row < EFF_BOARD_SIZE; row += 1){
        # sb.append("|");
        for (var col = 0; col < EFF_BOARD_SIZE; col += 1){
            sb.append(ICONS[boardCopy[row][col]]);
        }
        sb.append("\n");
    }
    clear();
    # print "PIECE POS: " + piecePos;
    print sb;
    print("Next piece:\n");
    var s = StringBuilder("");
    for (var i = 0; i < len(nextPiece); i += 1){
        for (var j = 0; j < len(nextPiece[0]); j += 1){
            s.append(ICONS[nextPiece[i][j]]);
        }
        s.append("\n");
        # print(ICONS[nextPiece[i]]);
    }
    print(s);

    print("Quick play instructions:\n");
    print(" - a - move piece left");
    print(" - d - move piece right");
    print(" - w - rotate piece counter clockwise");
    print(" - s - rotate piece clockwise");

    print(" - q - to quit the game anytime");

    if (errMsg)
        print(errMsg);
}

fn createBoard(){
    var board = [EFF_BOARD_SIZE]{0};
    for (var i = 0; i < EFF_BOARD_SIZE; i += 1){
        board[i] = [EFF_BOARD_SIZE]{0};
    }
    for(var i = 0; i < EFF_BOARD_SIZE; i += 1){
        board[i][0] = 1;
        board[EFF_BOARD_SIZE - 1][i] = 1;
        board[i][EFF_BOARD_SIZE - 1] = 1;
    }

    return board;
}

fn copyBoard(board){
    var ret = createBoard();
    for (var i = 0; i < EFF_BOARD_SIZE; i += 1){
        for (var j = 0; j < EFF_BOARD_SIZE; j += 1){
            ret[i][j] = board[i][j];
        }
    }
    return ret; 
}

fn getRandomPiece(){
    var index = randInt(0, len(PIECES) - 1);
    return PIECES[index];
}

fn getRandomPosition(currPiece){
    var currPieceSize = len(currPiece);
    return {0, randInt(1, EFF_BOARD_SIZE - currPieceSize - 1)};
}

fn isGameOver(board, currPiece, piecePos){
    return !canMoveDown(board, currPiece, piecePos) && !piecePos[0]; 
}

fn getLeftMove(piecePos){
    return {piecePos[0], piecePos[1] - 1};
}

fn getRightMove(piecePos){
    return {piecePos[0], piecePos[1] + 1};
}

fn getDownMove(piecePos){
    return {piecePos[0] + 1, piecePos[1]};
}

fn rotateClockwise(piece){
    var x = len(piece);
    var y = len(piece[0]);
    var ret = [y]{0};
    for (var i = 0; i < y; i += 1){
        ret[i] = [x]{0};
    }
    for (var i = 0; i < x; i += 1){
        for (var j = 0; j < y; j += 1){
            ret[j][x - i - 1] = piece[i][j];
        }
    }
    return ret;
}

fn rotateAntiClockwise(piece){
    var x = len(piece);
    var y = len(piece[0]);
    var ret = [y]{0};
    for (var i = 0; i < y; i += 1){
        ret[i] = [x]{0};
    }
    for (var i = 0; i < x; i += 1){
        for (var j = 0; j < y; j += 1){
            ret[y - j - 1][i] = piece[i][j];
        }
    }
    return ret;
}

fn mergeBoardAndPiece(board, currPiece, piecePos){
    var currPieceX = len(currPiece);
    var currPieceY = len(currPiece[0]);
    for(var x = 0; x<currPieceX; x+=1){
        for(var y = 0; y<currPieceY; y+=1){
            board[piecePos[0] + x][piecePos[1] + y] = currPiece[x][y] | board[piecePos[0] + x][piecePos[1] + y];
        }
    }
    # var emptyRow = [EFF_BOARD_SIZE]{0};
    # emptyRow[0] = 1;
    # emptyRow[EFF_BOARD_SIZE - 1] = 1;

    var filledRow = [EFF_BOARD_SIZE]{1};

    var filledRows = 0;
    for (var i = 0; i < EFF_BOARD_SIZE - 1; i += 1){
        if (board[i] == filledRow){
            filledRows += 1;
        }
    }
    
    return filledRows;

    # var g = ;

    
        # board.addAtIndex(1, emptyRow);
}

fn overlapCheck(board, currPiece, piecePos){
    #    True  - if piece do not overlap with any other piece or walls
    #    False - if piece overlaps with any other piece or board walls
    var currPieceX = len(currPiece);
    var currPieceY = len(currPiece[0]);
    # print "PIECE POS: " + piecePos;
    # print "PIECE: " + currPiece;
    for(var x = 0; x<currPieceX; x+=1){
        for(var y = 0; y<currPieceY; y+=1){
            if (currPiece[x][y] == 1 && board[piecePos[0] + x][piecePos[1] + y] == 1){
                # print "OVERLAP: " + (piecePos[0] + x) + " " + (piecePos[1] + y);
                # print "OVERLAP: " + board[piecePos[0] + x];
                # print "OVERLAP: " + board[piecePos[0] + x][piecePos[1] + y];
                return false;
            }
        }
    }
    return true;
}


fn canMoveLeft(board, currPiece, piecePos){
    var leftMove = getLeftMove(piecePos);
    return overlapCheck(board, currPiece, leftMove);
}

fn canMoveRight(board, currPiece, piecePos){
    var rightMove = getRightMove(piecePos);
    return overlapCheck(board, currPiece, rightMove);
}

fn canMoveDown(board, currPiece, piecePos){
    var downMove = getDownMove(piecePos);
    # print "DOWN MOVE: " + downMove;
    return overlapCheck(board, currPiece, downMove);
}

fn canRotateClockwise(board, currPiece, piecePos){
    var rotatedPiece = rotateClockwise(currPiece);
    return overlapCheck(board, rotatedPiece, piecePos);
}

fn canRotateAntiClockwise(board, currPiece, piecePos){
    var rotatedPiece = rotateAntiClockwise(currPiece);
    return overlapCheck(board, rotatedPiece, piecePos);
}

fn clearFilledRows(board, filledRows){
    if (filledRows > 0){
        var filledRow = [EFF_BOARD_SIZE]{1};
        board = Arrays.reduce(board[:-2], 
            fn (x) => x == filledRow;
        );
        var emptyRows = [filledRows];
        for(var i = 0; i < filledRows; i += 1){
            var emptyRow = [EFF_BOARD_SIZE]{0};
            emptyRow[0] = 1;
            emptyRow[EFF_BOARD_SIZE - 1] = 1;
            emptyRows[i] = emptyRow;
        }

        board = emptyRows + board + {filledRow};

    }

    return board;
}

fn play(){
    var board = createBoard();
    var score = 0;
    var currPiece = getRandomPiece();
    var nextPiece = getRandomPiece();
    var piecePos = getRandomPosition(currPiece);
    printBoard(board, score, currPiece, nextPiece, piecePos, null);
    while(true && !isGameOver(board, currPiece, piecePos)){     
        var moveDown = true;
            var error_message = null;
        if(kbhit()){
            var move = chr(getc());

            switch(move){
                case MOVE_LEFT{
                    if (canMoveLeft(board, currPiece, piecePos)){
                        piecePos = getLeftMove(piecePos);
                        moveDown = true;
                    }else{
                        error_message = "Cannot move left";
                    }
                }
                case MOVE_RIGHT{
                    if (canMoveRight(board, currPiece, piecePos)){
                        piecePos = getRightMove(piecePos);
                        moveDown = true;

                    }else{
                        error_message = "Cannot move right";
                    }
                }
                case ROTATE_ANTICLOCKWISE{
                    if (canRotateAntiClockwise(board, currPiece, piecePos)){
                        currPiece = rotateAntiClockwise(currPiece);
                        moveDown = true;
                    }else{
                        error_message = "Cannot rotate anti-clockwise";
                    }
                }
                case ROTATE_CLOCKWISE{
                    if (canRotateClockwise(board, currPiece, piecePos)){
                        currPiece = rotateClockwise(currPiece);
                        moveDown = true;
                    }else{
                        error_message = "Cannot rotate clockwise";
                    }
                }
                case NO_MOVE{
                    if (canMoveDown(board, currPiece, piecePos)){
                        piecePos = getDownMove(piecePos);
                    }else{
                        error_message = "Cannot move down";
                    }
                }
                case QUIT_GAME{
                    return;
                }
                default{
                    error_message = "Invalid move";
                }
            }
        }
        if(moveDown && canMoveDown(board, currPiece, piecePos)){
            piecePos = getDownMove(piecePos);
        }
        if (!canMoveDown(board, currPiece, piecePos)){
            var filledRows = mergeBoardAndPiece(board, currPiece, piecePos);
            score += filledRows;
            board = clearFilledRows(board, filledRows);
            currPiece = nextPiece;
            nextPiece = getRandomPiece();
            piecePos = getRandomPosition(currPiece);
        }
        printBoard(board, score, currPiece, nextPiece, piecePos, error_message);
        sleep(0.2);
    }
    print("GAME OVER");
}
# -------------------------------------------------------------------------------
var rows = 10;
var cols = 10;
var line = " " + join([cols]{"---"}, "") + " ";

class Field{

    init(rows, cols){
        this.rows = rows;
        this.cols = cols;
        this.icons = {" . "," * ", " # "," & "};
        this.board = this.create_board(rows, cols);
        this.snakeCoords = ArrayList();
        this.entityPos = {-1, -1};
        this.addEntity();
    }
    create_board(r, c){
        var ret = [r]{0};
        for (var i = 0; i < r; i += 1){
            ret[i] = [c]{0};
        }
        return ret;
    }
    addEntity(){
        while(true){
            var x = randInt(0, this.cols-1);
            var y = randInt(0, this.rows-1);
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
        for (var row = 0; row < this.rows; row += 1){
            for (var col = 0; col < this.cols; col += 1){
                if(this.board[row][col] == 1 || this.board[row][col] == 2)
                    this.board[row][col] = 0;
            }
        }
    }

    toStr(){
        this.clearBoard();
        for (var i = 0; i < this.snakeCoords.size(); i += 1){
            var coord = this.snakeCoords.get(i);
            this.board[coord[0]][coord[1]] = 1;
        }
        var head = this.snakeCoords.getLast();
        if(head)
            this.board[head[0]][head[1]] = 2;

        var sb = StringBuilder(line).append("\n");
        for (var row = 0; row < this.rows; row += 1){
            sb.append("|");
            for (var col = 0; col < this.cols; col += 1){
                sb.append(this.icons[this.board[row][col]]);
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
        for (var i = 0; i < this.coords.size() - 1; i += 1){
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
        # print "SNAKE: " + this.coords  + " SIZE: " + this.coords.size();
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
        coord[0] = (coord[0]) % cols;
        coord[1] = (coord[1]) % rows;
        if(coord[0] < 0){
            coord[0] = cols - 1;
        }
        if(coord[1] < 0){
            coord[1] = rows - 1;
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

# var name = input("HELLO! WHAT IS YOUR NAME?\n");
var name = "SNAKE";
var f = Field(rows, cols);
var s = Snake(name);
s.setField(f);

fn run (){
    while(true){
        var died = s.move();
        print "NAME: " + s.name;
        print "SCORE: " + s.score;
        print(f);
        print("<p> to pause -- <q> to quit");
        if(kbhit()){
            var d = getKeyInput();
            if(d == 112){
                clear();
                print "NAME: " + s.name;
                print "SCORE: " + s.score;
                print(f);
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
                    # clear();
                    sleep(0.016);
                }
            }else if(d == 113){
                return "QUIT";
            }else{
                s.setDirection(d);
            }
        }
        # sleep(0.016);
        # f.toStr();
        if(died) return "DIED";
        clear();
        sleep(0.1);
    }
}

# var g = run();

# if (g == "QUIT"){
#     print "GOODBYE!";
# }else if (g == "DIED"){
#     print "YOU DIED";
# }
play();


# if (getc() == 27){
    
#         case 97 {
#             print "a";
#         }
#         case 119{
#             print "w";
#         }
#         case 115{
#             print "s";
#         }
#         case 100{
#             print "d";
#         }
#     }
