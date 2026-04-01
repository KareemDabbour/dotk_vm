import "lang"
import "file"

var rows = 20;
var cols = 20;
var line = " ${} ".f("".join([cols]{"-"}))
var mapfn = fn(x) => bool(int(x)) ? "▓" : " "
# var string;
class GameOfLife{
    init(rows, cols, initial_state=null){
        this.rows = rows;
        this.cols = cols;
        this.board = create_board(rows, cols);
        if (initial_state != null){
            this.loadInitialState(initial_state);
        }else{
            this.randomizeBoard();
        }
    }
    loadInitialState(initial_state){
        # Load initial state from file or other source
        with(File(initial_state), fn(f){
            var lines = f.readLines();
            this.board = [len(lines)];
            lines
            .map(trim)
            .map(list)
            .foreach(fn(i, line){
                this.board[i] = line.map(mapfn);
            })
            this.cols = len(lines[0]);
            this.rows = len(lines);

        });
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
            print(f"|{"".join(this.board[row])}|");
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
        # var ret = create_board(this.rows, this.cols);
        var n = 0;
        for (var row = 0; row < this.rows; row += 1){
            for (var col = 0; col < this.cols; col += 1){
                # ret[row][col] = " ";
                n = this.get_side_ones(row, col);
                
                if (this.board[row][col] == "▓"){
                    if( n < 2) {
                        this.board[row][col] = " ";
                    }
                    if (n == 2 || n == 3){
                        this.board[row][col] = "▓";
                    }
                    if (n > 3) {
                        this.board[row][col] = " ";
                    }
                }else{
                    if (n == 3){
                        this.board[row][col] = "▓";
                    }
                }
            }
        }
        # this.board = ret;
    }
    gen2(){
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
        this.gen2();
        sleep(0.05);
        clear();
    }
    run(end_time=null){
        if (end_time == null){
            while(true){
                this.step();
            }
        }else{
            while(clock() < end_time){
                this.step();
            }
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
var start = clock();
var initial_state;

var i = ARG_V.indexOf("-is")
if (i != -1) {
    initial_state = ARG_V[i + 1];
    # Load initial state from file or other source
}


GameOfLife(rows, cols, initial_state).run()#start + 0.5);