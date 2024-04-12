import "stdlib.k";
var BOARD_SIZE = 15;
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
    for(var x = 0; x < currPieceX; x+=1){
        for(var y = 0; y < currPieceY; y+=1){
            boardCopy[piecePos[0] + x][piecePos[1] + y] = currPiece[x][y] | boardCopy[piecePos[0] + x][piecePos[1] + y];
        }
    }
    var sb = StringBuilder("SCORE: ").append(score).append("\n");
    for (var row = 0; row < EFF_BOARD_SIZE; row += 1){
        for (var col = 0; col < EFF_BOARD_SIZE; col += 1){
            sb.append(ICONS[boardCopy[row][col]]);
        }
        sb.append("\n");
    }
    clear();
    # print "PIECE POS: " + piecePos;
    print sb;
    print("Next piece:\n");
    var s = StringBuilder();
    for (var i = 0; i < len(nextPiece); i += 1){
        for (var j = 0; j < len(nextPiece[0]); j += 1){
            s.append(ICONS[nextPiece[i][j]]);
        }
        s.append("\n");
    }
    print s

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
    var board = [EFF_BOARD_SIZE]{0}.map(fn (x) => [EFF_BOARD_SIZE]{0});
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

    var filledRow = [EFF_BOARD_SIZE]{1};

    var filledRows = 0;
    for (var i = 0; i < EFF_BOARD_SIZE - 1; i += 1){
        if (board[i] == filledRow){
            filledRows += 1;
        }
    }
    
    return filledRows;
}

fn overlapCheck(board, currPiece, piecePos){
    #    True  - if piece do not overlap with any other piece or walls
    #    False - if piece overlaps with any other piece or board walls
    var currPieceX = len(currPiece);
    var currPieceY = len(currPiece[0]);
    for(var x = 0; x<currPieceX; x+=1){
        for(var y = 0; y<currPieceY; y+=1){
            if (currPiece[x][y] == 1 && board[piecePos[0] + x][piecePos[1] + y] == 1){

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
        board = Arrays.removeIf(board[:-2], 
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
                        moveDown = false;
                    }else{
                        error_message = "Cannot move left";
                    }
                }
                case MOVE_RIGHT{
                    if (canMoveRight(board, currPiece, piecePos)){
                        piecePos = getRightMove(piecePos);
                        moveDown = false;

                    }else{
                        error_message = "Cannot move right";
                    }
                }
                case ROTATE_ANTICLOCKWISE{
                    if (canRotateAntiClockwise(board, currPiece, piecePos)){
                        currPiece = rotateAntiClockwise(currPiece);
                        moveDown = false;
                    }else{
                        error_message = "Cannot rotate anti-clockwise";
                    }
                }
                case ROTATE_CLOCKWISE{
                    if (canRotateClockwise(board, currPiece, piecePos)){
                        currPiece = rotateClockwise(currPiece);
                        moveDown = false;
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


play();