
var rows = 50;
var cols = 25;
var line = " ${} ".f("".join([cols]{"-"}))
# var string;
class GameOfLife{
    init(rows, cols){
        this.rows = rows;
        this.cols = cols;
        this.board = create_board(rows, cols);
        this.randomizeBoard();
    }
    randomizeBoard(){
        for (var row = 0; row < this.rows; row += 1){
            for (var col = 0; col < this.cols; col += 1){
                if (randInt(0, 1)){
                    this.board[row][col] = "▓";
                }else{
                    this.board[row][col] = " ";
                }
            }
        }
    }
    draw_board(){
        # var string = "";
        print(line);
        for (var row = 0; row < this.rows; row += 1){
            print("|${}|".f("".join(this.board[row])));
        }
        print(line);
    }
    get_side_ones(row, col){
        var total_alive = 0;  
        for (var i = row - 1;i < row + 2 ; i += 1){
            for(var j = col - 1; j < col + 2; j += 1){
                if (i != row || j != col) {
                    if (this.board[i%this.rows][j%this.cols] == "▓") {
                        total_alive += 1;
                    }
                }
            }
        }
        return total_alive;
    }
    gen(){
        var ret = create_board(this.rows, this.cols);
        var n = 0;
        for (var row = 0; row < this.rows; row += 1){
            for (var col = 0; col < this.cols; col += 1){
                ret[row][col] = " ";
                n = this.get_side_ones(row, col);
                
                if (this.board[row][col] == "▓"){
                    if( n < 2) {
                        ret[row][col] = " ";
                    }
                    if (n == 2 || n == 3){
                        ret[row][col] = "▓";
                    }
                    if (n > 3) {
                        ret[row][col] = " ";
                    }
                }else{
                    if (n == 3){
                        ret[row][col] = "▓";
                    }
                }
            }
        }
        this.board = ret;
    }
    step(){
        this.draw_board();
        this.gen();
        sleep(0.05);
        clear();
    }
    run(){
        while(true){
            this.step();
        }
    }
}
fn create_board(r, c){
    var ret = [r]{0};
    for (var i = 0; i < r; i += 1){
        ret[i] = [c]{0};
    }
    return ret;
}

fn randomizeBoard(board){
    for (var row = 0; row < rows; row += 1){
        for (var col = 0; col < cols; col += 1){
            board[row][col] = randInt(0, 1);
        }
    }
    # return board;
}

fn draw_board(board){
    var string = "";
    print(line);
    for (var row = 0; row < rows; row += 1){

        string = "|";
        for (var col = 0; col < cols; col += 1){
            if(board[row][col]){
                string += "▓";
            }else{
                string += " ";
            }
        }
        string += "|";
        print(string);
    }
    print(line);
}

fn get_side_ones(board, row, col){
    var total_alive = 0;  
    for (var i = row - 1;i < row + 2 ; i += 1){
        for(var j = col - 1; j < col + 2; j += 1){
            if (i != row || j != col) {
                if (board[i%rows][j%cols]) {
                    total_alive += 1;
                }
            }
        }
    }
    return total_alive;
}

fn gen(board){
    var ret =create_board(rows, cols);
    var n = 0;
    for (var row = 0; row < rows; row += 1){
        for (var col = 0; col < cols; col += 1){
            ret[row][col] = 0;
            n = get_side_ones(board, row, col);
            
            if (board[row][col] == 1){
                if( n < 2) {
                    ret[row][col] = 0;
                }
                if (n == 2 || n == 3){
                    ret[row][col] = 1;
                }
                if (n > 3) {
                    ret[row][col] = 0;
                }
            }else{
                if (n == 3){
                    ret[row][col] = 1;
                }
            }
        }
    }
    # return ret;
    board = ret;
}

fn run(){
    draw_board(b);
    gen(b);
    # sleep(0.1);
    clear();
}

# var b = create_board(rows, cols);
# randomizeBoard(b);

# while(true){
#     run();
# }

GameOfLife(rows, cols).run();