~
class Pair {
    init(first, second){
        this.first = first;
        this.second = second;
    }

    toStr(){
        return f"({this.first}, {this.second})"#.f(this.first, this.second);
    }
    _eq_(other){
        return this.first == other.first && this.second == other.second;
    }
    _hash_(){
        return hash(this.first) + hash(this.second);
    }
}

class GraphPair<Pair>{

    init(){
        super.init(1,fn()=>1);
    }

    toStr(){
        return f"{super.toStr()} {this.second()}";
    }
  _lt_(other){
    return this.second < other.second;
  }
  _gt_(other){
    return this.second > other.second;
  }
}

var p = GraphPair();
print p

class Graph {
    init(){
        this.adjList = HashMap();
    }

    addEdge(u, v){
        this.adjList.compute(u, fn (k, val) => val ? val.append(v) : {v});
    }

    _get_(u){
        return this.adjList[u];
    }

    _set_(u, v){
        this.addEdge(u, v);
    }

    _hash_(){
        return hash(this.adjList);
    }

    minDist(dist, sptSet){
        var min = Math.INF;
        var minIndex = -1;
        dist.keys().foreach(fn (i) {
            if (!sptSet.contains(i) && (dist[i]) < min){
                min = dist[i];
                minIndex = i;
            }
        });
        return minIndex;
    }

    dijkstra(src){
        var dist = HashMap();
        this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
        dist[src] = 0;
        # print("dist: ${}".f(dist));
        var sptSet = Set();
        for (var i = 0; i < len(this.adjList); i+=1){
            var u = this.minDist(dist, sptSet);
            # print("u: ${}".f(u));
            sptSet.add(u);
            
            var adj = this.adjList.get(u, []);
            print "adj: ${}".f(adj);
            adj.foreach(fn (v) {
                # print "v: ${}".f(v);
                if (!sptSet.contains(v) && dist[u] != Math.INF && (dist[u] + 1) < dist.get(v, Math.INF)){
                    dist[v] = dist[u] + 1;
                }
            });
        }
        return dist;

    }

    toStr(){
        var s = StringBuilder();
        this.adjList.entries().foreach(fn (e) {
            s.append(e[0]).append(" -> ").append(e[1]).append("\n");
        });
        return s.toStr();
        # return this.adjList.toStr();
    }
}

class WeightedDirectedGraph <Graph> {
    init(){
        this.adjList = HashMap();
    }

    addEdge(u, v, w){
        this.adjList.compute(u, fn (k, val) => val ? val.append(GraphPair(v, w)) : {GraphPair(v, w)});
    }

    minDist(dist, sptSet){
        var min = Math.INF;
        var minIndex = -1;
        var d = dist.keys();
        for(var s = 0; s < len(d); s += 1){
            var i = d[s];
            if (!sptSet.contains(i) && (dist[i]) < min){
                min = dist[i];
                minIndex = i;
            }
        }
        return minIndex;
    }

    dijkstra(src){
        var dist = HashMap();
        this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
        dist[src] = 0;
        # print("dist: ${}".f(dist));
        # print "adjList: ${}".f(this.adjList);
        var sptSet = [];
        for (var i = 0; i < len(this.adjList.keys()); i+=1){
            var u = this.minDist(dist, sptSet);
            # print("u: ${}".f(u));
            sptSet.append(u);
            
            var adj = this.adjList.get(u, []);
            # print "adj: ${}".f(adj);

            
            adj.foreach(fn (vP) {
                var v = vP.name;
                var d = dist[u];
                # print "v: ${}".f(v);
                # print "dist[u]: ${}".f(d);
                # print "dist[v]: ${}".f(dist.get(v, Math.INF));
                # print "spSet: ${}".f(sptSet);
                if (!sptSet.contains(v) && d != Math.INF && (d + 1) < dist.get(v, Math.INF)){
                    dist[v] = (dist[u] + 1);
                }
            });
        }
        return dist;

    }

    shortestPath(src, dest){
        var dist = this.dijkstra(src);
        return dist[dest];
    }



    toStr(){
        var s = StringBuilder();

        var d = this.adjList.entries()
        
        d.foreach(fn (e) {
            s.append(f"{e[0]} -> {e[1]}\n")#).append(" -> ").append(e[1]).append('\n');
        });
        
        return s.toStr();
        # return this.adjList.toStr();
    }

}


var STDIN = 0
var STDOUT = 1
var STDERR = 2


var g = WeightedDirectedGraph();
g.addEdge("A", "B", 1);
g.addEdge("A", "C", 1);
g.addEdge("B", "D", 1);
g.addEdge("C", "D", 1);
g.addEdge("B", "A", 1);


print g
~

# fn inputMode(){
#     if (s.key == 13 && s.mod == 2 && s.type == 1){
#         x = -1;
#         y += 1;
#     }else if (s.key == 127 && s.mod == 2 && s.type == 1){
#         x -= 1;
#         tP(x, y, 0, 0, " ");
#         tPres();
#     }else{
#         tP(x+=1,y,0,0,chr(s.ch));
#         tPres();
#     }
#     s = tPoll();
# }



# tI();
# # print ("TESTING DIJKSTRA");
# tP(0,0,0,0,"WELCOME!\n");
# tPres();
# var y = 1;
# var x = -1;
# var s = tPoll();
# while(s.mod != 2 || s.key != 3 || s.type != 1){
#     # tP(0,y+=1,0,0,s.toStr());
#     # tPres();
#     # s = tPoll();
#     if (s.key == 13 && s.mod == 2 && s.type == 1){
#         x = -1;
#         y += 1;
#     }else if (s.key == 127 && s.mod == 2 && s.type == 1){
#         tP(x, y, 0, 0, " ");
#         x -= 1;
#         tPres();
#     }else if(s.key == 0){
#         tP(x+=1,y,0,0,chr(s.ch));
#         tPres();
#     }
#     s = tPoll();
# }
# # tClear();

# print("ADADADASD")
# tPres();
# var s = tPoll();
# tC();

# import "stdlib"
# import "lang"

# var input = 
# `3   4
# 4   3
# 2   5
# 1   3
# 3   9
# 3   3`

# print "PART 1"

# input = with(File("./aoc2024/1"), fn (f) => f.read());

# var left = []
# var right = []


# var lines = input.split("\n").foreach(fn (line) {
#     var lineArr = line.split("   ");
#     left.append(int(lineArr[0]));
#     right.append(int(lineArr[1]));
#     })
    
# Arrays.mergeSort(left);
# Arrays.mergeSort(right);

# var dist = 0;
# left.foreach(fn (index, l) {
#     dist = dist + Math.abs(l - right[index]);
# });

# print f"dist: {dist}\n"


# print "PART 2"

# var rightMap = HashMap();

# right.foreach(fn (num) {
#     rightMap.compute(num, fn(k, v) => v ? v + 1 : 1);
# });


# var sim = 0;

# left.foreach(fn(num){
#   sim += (num * rightMap.get(num, 0));  
# })

# print f"similarity: {sim}"

# import "stdlib"
~
var input =
`7 6 4 2 1
1 2 7 8 9
9 7 6 2 1
1 3 2 4 5
8 6 4 4 1
1 3 6 7 9`

input = with(File("./2"), fn (f) => f.read());

var lines = input.split("\n").map(fn(l) => l.split(" ").map(int));

fn checkLine(line){
    var isDec = line[0] > line[1];
    for (var i = 0; i < len(line) - 1; i += 1){
        if (line[i] == line[i + 1]){
            return false;
        }
        if (isDec && (line[i] < line[i + 1])){
            return false;
        }
        if (!isDec && (line[i] > line[i + 1])){
            return false;
        }
        if (Math.abs(line[i] - line[i + 1]) > 3){ 
            return false;
        }

    }
    return true;
}



var s = 0;
lines.foreach(fn (line) {
    if (checkLine(line)){
        s += 1;
    }
})

# print f"valid: {s}"

fn checkPair(i, x, direction){
    if (i == x){
        return false;
    }
    # -1 is ascending
    # 1 is descending
    if ((i - x) * direction < 0){
        return false;
    }

    if (Math.abs(i - x) > 3){
        return false;
    }

    return true;
}

class Direction{
    ASC = -1;
    DESC = 1;
    INVALID = 0;
}


fn getDir(line){
    var  inc = 0 
    var dec = 0
    var eq = 0
    for (var i = 0; i < len(line) - 1; i += 1){
        if (line[i] == line[i + 1]){
            eq += 1;
        }else
        if (line[i] > line[i + 1]){
            dec += 1;
        }else
        if (line[i] < line[i + 1]){
            inc += 1;
        }
    } 
    # print f"inc: {inc} dec: {dec} eq: {eq}";
    if (eq > 1 || (inc > 1 && dec > 1)){
        # print f"failed due to eq: {eq} inc: {inc} dec: {dec}";
        return Direction.INVALID;
    }
    if (inc > dec){
        return Direction.ASC;
    }
    else{
        return Direction.DESC;
    }
}

var numInvalid = 0;

fn linechecker(line){
    var i  = 0;
    var encounteredErrorAt = -1;
    var dir = getDir(line);
    if (dir == Direction.INVALID){
        numInvalid += 1;
        return false;
    }


#    54 56 57 58
# 
    var c = 0;
    for (var i = 0; i < len(line) - 1; i += 1){
        # print f"prev: {prev} line[i]: {line[i]} c: {c}";
        if (c > 1){
            return false;
        }
        if (!checkPair(line[i], line[i + 1], dir)){
            if (i >= 1 && checkPair(line[i - 1], line[i + 1], dir)){
                # print f"works at: {i} {line[i-1]} {line[i + 1]}";
                c += 1;
                continue
            }
            if ((i < len(line) - 2) && checkPair(line[i], line[i + 2], dir)){
                # d = true;
                # print f"works at: {i} {line[i]} {line[i + 2]}";
                # return false;
                c += 1;
                i+=1;
                continue
            }
            if (i < len(line) - 2 && checkPair(line[i + 1], line[i + 2], dir)){
                # print f"works at: {i} {line[i]} {line[i + 2]}";
                c += 1;
                continue
            }
            return false;
        }
        
    }
    return true
}

var s = 0;

# lines.foreach(fn (index, line) {
#     if (linechecker(line)){
#         # print f"\tI WORK: {index}"
#         s += 1;
#     }
#  })

# linechecker(lines[3])
print f"valid: {s}"

print f"invalid: {numInvalid}"



print f"total: {len(lines)}"

print f"t: {linechecker("1 3 6 5 7 10".split(" ").map(int))}"
~
# rules has to only be decreasing or increasing and by no more than 2 and no less than 1
~
import "stdlib"
var input = 
`xmul(2,4)%&mul[3,7]!@^do_not_mul(5,5)+mul(32,64]then(mul(11,8)mul(8,5))`

input = `xmul(2,4)&mul[3,7]!^don't()_mul(5,5)+mul(32,64](mul(11,8)undo()?mul(8,5))`
input = with(File("./3"), fn (f) => f.read());

var re1 = `mul\([0-9]+,[0-9]+\)`
var re2= `mul\([0-9]+,[0-9]+\)|do\(\)|don't\(\)`

var muls = input.match(re2) #.match()
# print f"muls: {muls}"

var do = true;
var tot = 0;

muls.foreach(fn (match){
    switch (match){
        case ("do()")
        {
            do = true;
            return;
        }
        case ("don't()")
        {
            do = false;
            return;
        }
        default 
        {
            if (do){
                # print f"match: {match}"
                tot += reduce(match.match(`[0-9]+`).map(int), fn(a, b) => a * b, 1)

                 #(nums[0] * nums[1]);
            }
        }
    }
})
print f"tot: {tot}"
# print f"tot: {reduce(muls.map(fn (m) =>reduce(m.match(`[0-9]+`).map(int), fn(a, b) => a * b, 1)), fn(a, b) => a + b, 0)}"


actually, make it the map of nums that have to be b4 the current num
{
47: [53, 13, 61, 29],
97: [13, 61, 47, 29, 75, 53]
}

and then loop through the page ordering and check if the 


import "stdlib"

var input = 
`47|53
97|13
97|61
97|47
75|29
61|13
75|53
29|13
97|29
53|29
61|53
97|53
61|29
47|13
75|47
97|75
47|61
75|61
47|29
75|13
53|13

75,47,61,53,29
97,61,53,29,13
75,29,13
75,97,47,61,53
61,13,29
97,13,75,29,47`

input = with(File("./5"), fn (f) => f.read());

var sections= input.split("\n\n")
var ruleArr = [100].map(fn(i) => [100]{false})
var rules = sections[0].split("\n").map(fn (line) => line.split("|").map(int))
rules.foreach(fn(rule) {
 ruleArr[rule[0]][rule[1]] = true
})
var orders = sections[1].split("\n")#.map(fn (line) => line.split(",").map(int))

fn sort(order){
    var s = 
    order
    .split(",")
    .map(int)
    .sort(
        fn(a,b) {
            if(a == b) return 0;
            return ruleArr[a][b]?-1:1;
        })

    return s#[len(s)//2].val;
}


var tot = 0;
print sort("97,13,75,29,47")
print sort("75,97,47,61,53")
muls
.map(fn (m) => 
    m.match(`[0-9]+`)
     .map(int)
     .reduce(fn(a, b) => a * b, 1))
.reduce(fn(a, b) => a + b, 0)
# orders.foreach(fn(i, order){
#     var isValid = true;
#     for (var j = 0; j < len(rules); j += 1){
#         if (order.match(`${}\S*${}`.f(rules[j][1], rules[j][0]))){
#             isValid = false;
#             break;
#         }
#     }
#     if (!isValid){
#         tot += sort(order)
#     }
# })
# print f"tot {tot}"




import "lang"

var input =
`MMMSXXMASM
MSAMXMSMSA
AMXSXMAAMM
MSAMASMSMX
XMASAMXAMM
XXAMMXXAMA
SMSMSASXSS
SAXAMASAAA
MAMMMXMMMM
MXMXAXMASX`




input = with(File("./4"), fn (f) => f.read());

var board = input.split("\n").map(list)
# check every direction in star pattern
var XMAS = "XMAS"
var MAS = "MAS"

var dirs = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}



var totp1 = 0 
var totp2 = 0


fn checkDir(board, x, y, dx, dy){
    var index = 1;
    for (var i = 1; i < 4; i += 1){
        var nx = x + (dx * i);
        var ny = y + (dy * i);
        if (nx < 0 || nx >= len(board[0]) || ny < 0 || ny >= len(board)){
            return 0;
        }
        if (board[ny][nx] != XMAS[index]){
            return 0;
        }
        index += 1;
        if (index == 4){
            break;
        }
    }
    return 1;
}

fn checkForXMAS(board, x, y){
    var xmas = 0;
    var index = 1;


    dirs.foreach(fn(dir){
        var dx = dir[0];
        var dy = dir[1];
        xmas += checkDir(board, x, y, dx, dy);
    })
    return xmas;

}

fn checkForCrossMAS(board, x, y){

    if (x == 0 || y == 0 || x == len(board[0]) - 1 || y == len(board) - 1){
        return 0;
    }

    if (   (board[y-1][x-1] != "M" && board[y+1][x+1] != "M")
        || (board[y-1][x-1] != "S" && board[y+1][x+1] != "S")
        || (board[y-1][x-1] == "S" && board[y+1][x+1] != "M")
        || (board[y-1][x-1] == "M" && board[y+1][x+1] != "S")){
        return 0;
    }
    
    if (   (board[y-1][x+1] != "S" && board[y+1][x-1] != "S")
        || (board[y-1][x+1] != "M" && board[y+1][x-1] != "M")
        || (board[y-1][x+1] == "S" && board[y+1][x-1] != "M")
        || (board[y-1][x+1] == "M" && board[y+1][x-1] != "S")){
        return 0;
    }

    return 1;
}


for (var y = 0; y < len(board); y += 1){
    var row = board[y];
    for (var x = 0; x < len(row); x += 1){
        if (row[x] == "X"){
            totp1 += checkForXMAS(board, x, y)
        }
        else 
        if (row[x] == "A"){
            totp2 += checkForCrossMAS(board, x, y)
        }
    }
}
print f"tot 4 p1: {totp1}\ntot 4 p2: {totp2}"
~


~
var s = {5,4,3,2,1}
var g = fn(a,b) { if (a == b) return 0; return b>a?1:-1}
s.sort(g)


import "lang"

var input =
`....#.....
.........#
..........
..#.......
.......#..
..........
.#..^.....
........#.
#.........
......#...`

var input = 
`...........#.....#......
...................#....
...#.....##.............
......................#.
..................#.....
..#.....................
....................#...
........................
.#........^.............
..........#..........#..
..#.....#..........#....
........#.....#..#......`

input = with(File("./6"), fn (f) => f.read());

var board = input.split("\n").map(list)

var index = input.find("^")
var x = index % (len(board[0]) + 1)
var y = index // (len(board[0]) + 1)

print f"x: {x} y: {y}"#" len: x: {len(board[0])} y: {len(board)}"

class Direction {
    UP = {-1,0};
    RIGHT = {0,1};
    DOWN = {1,0};
    LEFT = {0,-1};

    turnRight(dir){
        switch(dir){
            case (Direction.UP){
                return Direction.RIGHT;
            }
            case (Direction.RIGHT){
                return Direction.DOWN;
            }
            case (Direction.DOWN){
                return Direction.LEFT;
            }
            case (Direction.LEFT){
                return Direction.UP;
            }
        }
    }

}

fn inBounds(x1, y1){
    # print f"x1: {x1} y1: {y1} len: x: {len(board[0])} y: {len(board)}"
    return x1 >= 0 && x1 < len(board[0]) && y1 >= 0 && y1 < len(board);
}

class Gaurd {
    init(x, y, dir){
        this.x = x;
        this.y = y;
        this.dir = dir;
        # this.startDir = dir[:];
        this.looped = false;
        this.oob= false;
        this.locations = {{y, x}};
        this.path = {{y, x, dir}};
        this.extraObsticals = []
    }

    move(){
        
        var nx = this.x + this.dir[1];
        var ny = this.y + this.dir[0];
        if (inBounds(nx, ny)){
            # print f"c: {board[ny][nx]} nx: {nx} ny: {ny}"
            if (board[ny][nx] == "#" ){

                var prev = this.dir;
                this.dir = Direction.turnRight(this.dir);
                    nx = this.x + this.dir[1];
                    ny = this.y + this.dir[0];
                while(board[ny][nx] == "#" && this.dir != prev){
                    this.dir = Direction.turnRight(this.dir);
                    nx = this.x + this.dir[1];
                    ny = this.y + this.dir[0];
                }
                if(this.dir == prev){
                    this.oob = true;
                    return;
                }
                
                # print f"new dir: {this.dir} at this.extraObsticals: {this.extraObsticals}"
            }
  
            this.x += this.dir[1];
            this.y += this.dir[0];
            if (this.path.contains({this.y, this.x, this.dir})){
                this.looped = true;
            }
            
            
            if (!this.locations.contains({this.y, this.x})){
                this.locations.append({this.y, this.x});
            }
            
        }else{
            this.oob = true;
        }
        this.path.append({this.y, this.x, this.dir});
    }

    turnRight(){
        this.dir = Direction.turnRight(this.dir);
    }

    addObstical(pathPoint){
        var nx = pathPoint[1];
        var ny = pathPoint[0];

        if (inBounds(nx, ny)){
            if(nx != x || ny != y ){
                this.extraObsticals = {ny, nx};
                board[ny][nx] = "#";
            }else{
                return false;
            }
        }else{
            this.oob = true;
        }
        return true;
    }

    resetBoard(){
        if(this.extraObsticals){
            board[this.extraObsticals[0]][this.extraObsticals[1]] = ".";
        }
    }

}

var g = Gaurd(x, y, Direction.UP)

while (!g.looped && !g.oob){
    g.move();
}

print len(g.locations)
# print g

var looped = 0
var oob = 0
var r = 0
var obs=  []
var failed = HashMap()
# var edited = false
for (var i = 0; i < len(g.path) - 1; i += 1){

    var g2 = Gaurd(g.path[i][1], g.path[i][0], g.path[i][2]);
    # if (){
# print f"adding obstical at {g2.x}, {g2.y}"
    if (failed.containsKey(g.path[i+1][:2]) || !g2.addObstical(g.path[i+1])){
        continue;
    }

    while (!g2.looped && !g2.oob){
        g2.move();
    }

    if (g2.looped){
        # print(f"LOOPED: {g2.path}")
        if (!obs.contains(g2.extraObsticals)){
            obs.append(g2.extraObsticals)
        }
        looped += 1;
        failed[g2.extraObsticals] = true;
        # sleep(5)
    }

    if (g2.oob){
        failed[g2.extraObsticals] = false;
        oob += 1;
    }
    g2.resetBoard();
    if (i % 500 == 0){
        print f"i: {i} score: {len(obs)} | {looped} oob: {oob}"
    }

} 
# import "stdlib"
# print reduce(failed.entries(), fn(a, b) {
#     if (b[1]){
#         print "b: ${}".f(b)
#         return a + 1;
#     }
#     return a;
# }, 0)
print f"{len(g.path)} score: {len(obs)} {looped} oob: {oob}"
# print f"obs: {obs}"



import "lang"

var input =
`190: 10 19
3267: 81 40 27
83: 17 5
156: 15 6
7290: 6 8 6 15
161011: 16 10 13
192: 17 8 14
21037: 9 7 18 13
292: 11 6 16 20`

input = with(File("./t7"), fn (f) => f.read());

var lines = input.split("\n").map(fn(l) => l.split(": ")).map(fn(l) {
    var nums = l[1].split(" ").map(int);
    return {int(l[0]), nums};
})

# print lines 


var OP_ADD = "+"
var OP_MUL = "*"
var OP_CONCAT = "||"

var OPS = {OP_ADD, OP_MUL, OP_CONCAT}

var combCache = HashMap()

# get all permutations of nums and operators


fn product(it, repeat){
    fn helper(rest){
        if (!rest){
            return {{}};
        }
        var first = rest[0]
        var rest_ = helper(rest[1:])
        var ret = []
        first.foreach(fn(item){
            rest_.foreach(fn(sub){
                ret.append({item}.extend(sub))
            })
        })
        return ret;
    }

    fn mul(it, repeat){
        if (repeat == 1){
            return {it};
        } 
        return [repeat]{it}
    }
    
    return helper(mul(it, repeat))

}


fn permutations(nums ){

    var nPos = len(OPS)**(len(nums) - 1)
    var ret = [nPos].map(fn(i) => [])

    combCache.computeIfAbsent(nPos, fn (_) => product(OPS, len(nums) - 1))
    var combination = combCache[nPos]
    # print f"combination: {combination}"
    var combIndex = 0;
    for (var i = 0; i < len(nums) - 1; i+=1){
        for (var j = 0; j < nPos; j+=1){
            ret[j].append(nums[i]);
            ret[j].append(combination[j][i]);   
            # combIndex += 1;
        }
        # for (var c = 0; c < len(combination[combIndex]); c+=1){
        #     ret[c].append(combination[combIndex][c]);
        # }
        # for (var j = 0; j < len(nums) -1 ; j+=1){
        #     print f"j: {j} combIndex: {combIndex}"
        #     print f"combination[combIndex][j]: {combination[combIndex][j]}"
        #     ret[j].append(combination[combIndex][j]);
        # }

    }

    for (var j = 0; j < nPos; j+=1){
        ret[j].append(nums[-1]);
    }

    return ret 
}


var r = 0
lines.foreach(fn(l){
    # var prod = product(OPS, len(l[1]) - 1)
    var tot = l[0]
    var fund = false;
    permutations(l[1]).foreach(fn(p){
        # print p
        var sum = p[0]
        # go through and perform the concatenation
        
        var i = 1;
        while (i < len(p)){            
            if (p[i] == OP_ADD){
                sum += p[i + 1];
            }else if (p[i] == OP_MUL){
                sum *= p[i + 1];
            }else if (p[i] == OP_CONCAT){
            # print f"BEFORE: {p}"

                sum = int("${}${}".f(sum, p[i + 1]));

                p.pop(i + 1);
                p.pop(i);
                p[i - 1] = sum
                i -= 2;
            }
            # print f"UPDATE: {p} {sum} {i} {p[i]}"
            i += 1;
        }
        # print f"sum: {sum} I: {i}"s
        

        if (sum == tot && !fund){
            fund = true;
            r += tot;
            # print f"\t\tsum: {sum} p: {p}"
        }
    })

})

print f"r: {r}"


# print permutations({10, 19}, OPS)
# print permutations({10, 19, 9}, OPS)
# print permutations({10, 19, 9,3}, OPS)


# lines.foreach(fn(l){
#     if ()
# })












import "lang"


var input = 
`............
........0...
.....0......
.......0....
....0.......
......A.....
............
............
........A...
.........A..
............
............`


# input =
# `..........
# ..........
# ..........
# ....a.....
# ........a.
# .....a....
# ..........
# ..........
# ..........
# ..........`

input = with(File("./t8"), fn (f) => f.read());

var board = input.split("\n").map(list)


var freqPoints = HashMap()
var ch = []
var hashes = []
var numChecked = 0
board.foreach(fn(y, row){
    row.foreach(fn(x, char){
        # print f"char: {char}"
        if (char == '.'){
            return;
        }
        # if (!ch.contains(char)){
        #     ch.append(char);
        #     hashes.append(hash(char))#{hash(char), char})
            
        # }
        # var ss= freqPoints.get(char)
        # print f"c: {char} ss: {ss}"
        # if (!ss){
        #     freqPoints[char] = {{y, x}}
        # }
        # freqPoints[char].append({y, x})
        freqPoints.compute(char, fn(k, v) => v ? v.append({y, x}) : {{y, x}});
    })
})

# print freqPoints
print f"numChecked: {len(freqPoints.keys())}"
 
var entries = freqPoints.entries()




fn abs(a){
    return a < 0 ? -a : a;
}

fn manhattenDist (p1, p2){
    return {p2[0] - p1[0], p2[1] - p1[1]};
}


fn isInBounds(x, y){
    return x >= 0 && x < len(board[0]) && y >= 0 && y < len(board);
}

fn isPIn(point, offset){
    return isInBounds(point[1] + offset[1], point[0] + offset[0]) ;
}

fn isnPIn(point, offset){
    return isInBounds(point[1] - offset[1], point[0] - offset[0]) #&& {point[0] - offset[0],point[1] - offset[1]} != p;
}


var hash1 = HashMap()

entries.foreach(fn(entry){
    if (len(entry.value) < 2){
        return;
    }
    for (var i = 0; i < len(entry.value); i += 1){
        var p1 = entry.value[i];
        for(var j = i + 1; j < len(entry.value); j += 1){
            var p2 = entry.value[j];
            var dist = manhattenDist(p1, p2);
            var dy = p2[0] - p1[0];
            var dx = p2[1] - p1[1];

            var anti = {p1[0] - dy, p1[1] - dx}
            if (isInBounds(anti[1], anti[0])){
                hash1[anti] = true;
            }


            anti = {p2[0] + dy, p2[1] + dx}
            if (isInBounds(anti[1], anti[0])){
                hash1[anti] = true;
            }
            # print f"{entry.key}: dist: {dist} from {p1} to {p2}"
            # if (isPIn(p1, dist, p2)){
            #     # print f"{entry.key}: ANTINODE: {p1[0] + dist[0]} {p1[1] + dist[1]}"
            #     hash[{p1[0] + dist[0], p1[1] + dist[1]}] = true;
            # }
            # if(isnPIn(p1, dist)){
            #     # print f"{entry.key}: ANTINODE: {p1[0] - dist[0]} {p1[1] - dist[1]}"
            #     hash1[{p1[0] - dist[0], p1[1] - dist[1]}] = true;
            #     f.add({p1[0] - dist[0], p1[1] - dist[1]})
            #     gg.append({p1[0] - dist[0], p1[1] - dist[1]})
            # }

            # if (isPIn(p2, dist)){   
            #     # print f"{entry.key}: ANTINODE: {p2[0] + dist[0]} {p2[1] + dist[1]}"
            #     hash1[{p2[0] + dist[0], p2[1] + dist[1]}] = true;
            #     f.add({p2[0] + dist[0], p2[1] + dist[1]})
            #     gg.append({p2[0] + dist[0], p2[1] + dist[1]})
            # }
            
            # if(isnPIn(p2, dist, p1)){
            #     # print f"{entry.key}: ANTINODE: {p2[0] - dist[0]} {p2[1] - dist[1]}"
            #     hash[{p2[0] - dist[0], p2[1] - dist[1]}] = true;
            # }
        }
    }
})

print len(hash1.keys())


import "lang"

var input = list("2333133121414131402").map(int)

input = list(with(File("./t9"), fn (f) => f.read())).map(int)

var fileId=-1
var fileSize = HashMap()
var mem = []

input.foreach(fn (i, c){
    if ((i % 2) == 0){
        fileId += 1;
        fileSize[fileId] = c;
    }
    for (var j = 0; j < c; j += 1){
        mem.append((i % 2) == 0 ? fileId : null);
    }
})

# print 

var mem2 = mem
# print 
for (var i = len(mem2) - 1; i >= 0; i -= 1){
    if (mem2[i] != null){
        var size = fileSize[mem2[i]];
        print f"i: {i} mem2[i]: {mem2[i]} size: {size}"

        for (var j = 0;j < i;j+=1){
            if (mem2[j] == null){
                var end = j;
                while (end < len(mem2) && mem2[end] == null){
                    end += 1;
                }
                if (end - j >= size){
                    # print f"\tFOUND END: {end} j: {j} "
                    for (var k = 0; k < size; k += 1){
                        mem2[j + k] = mem2[i];
                    }
                    # print f"mem2: {mem2}"
                    for (var k = 0; k < size; k += 1){
                        mem2[i - k] = null;
                    }
                    
                    break;
                }
            }
        }
    }
    if (i % 1000 == 0){
        print f"processed i: { i } / { len(mem2) }"
    }
}

# print mem2



# for (var i = 0; i<len(mem2); i+=1){
#     var end = i;
#     if (mem2[i] == null){
#         while(end < len(mem2) && mem2[end] == null){
#             end += 1;
#         }
#         if (end - i > 1){
#             var size = end - i;
#             if (availFree.containsKey(size)){
#                 availFree[size].append(i);
#             }else{
#                 availFree[size] = {i};
#             }
#         }
#     }
# }

var tot2 = 0

mem2.foreach(fn(i, m){
    if (m != null){
        tot2 += m * i;
    }
})

print tot2

# print mem

# # PART 1
# for (var i = 0; i < len(mem); i += 1){
#     if (mem[i] == null){
#         var j = len(mem) - 1;
#         while (j > i && mem[j] == null){
#             j -= 1;
#         }
#         if (j > i){
#             mem[i] = mem[j];
#             mem[j] = null;
#         }
#     }
#     if (i % 1000 == 0){
#         print f"processed i: {i}"
#     }
# }




# # print mem
# print "files: " + fileSize
# print "frees: " + availFree

# var tot = 0

# mem.foreach(fn(i, m){
#     if (m != null){
#         tot += m * i;
#     }
# })

# print tot 



import "lang"

var input = 
`89010123
78121874
87430965
96549874
45678903
32019012
01329801
10456732`

input = with(File("./t10"), fn (f) => f.read());

var board = input.split("\n").map(fn (r) => list(r).map(int))

# print f"board: {board}"


fn isThereATo9(y,x, setOfPoints){

    var pos = board[y][x];
    # print f"{id} Checking: pos: {pos} y: {y} x: {x}"
    if (y < 0 || y >= len(board) || x < 0 || x >= len(board[0])){
        return 0 ;
    }
    if (pos == 9){
        # print "FOUND 9"
        setOfPoints.put({y, x}, true);
        return 1;
    }
    var sum = 0

    if (y < len(board) - 1 && pos + 1 == board[y + 1][x]){
        sum += isThereATo9( y + 1, x, setOfPoints);
    }
    if (y > 0 && pos + 1 == board[y - 1][x]){
        sum += isThereATo9( y - 1, x, setOfPoints);
    }
    if (x < len(board[0]) - 1 && pos + 1 == board[y][x+1]){
        sum += isThereATo9( y, x + 1, setOfPoints);
    }
    if (x > 0 && pos + 1 == board[y][x - 1]){
        sum += isThereATo9( y, x - 1, setOfPoints);
    }
    # print_error("damn")
    return sum;
}




var setOfPoints = HashMap()
var sum = 0;
var p2 = 0;
board.foreach(fn (y, row){
    row.foreach(fn (x, pos){
        setOfPoints.clear()
        if (pos == 0){
            p2 += isThereATo9(y, x, setOfPoints);
        }
        sum += len(setOfPoints);
    })
})

print f"setOfPoints: {sum}"
print f"p2: {p2}"


~

import "lang"


var input = 
`RRRRIICCFF
RRRRIICCCF
VVRRRCCFFF
VVRCCCJFFF
VVVVCJJCFE
VVIVCCJJEE
VVIIICJJEE
MIIIIIJJEE
MIIISIJEEE
MMMISSJEEE`

# input = 
# `AAAA
# BBCD
# BBCC
# EEEC`


# input = 
# `OOOOO
# OXOXO
# OOOOO
# OXOXO
# OOOOO`

input = with(File("./t12"), fn (f) => f.read());

var board = input.split("\n").map(list)
var checked = [len(board)].map(fn(_) => [len(board[0])]{false})



var regions = HashMap()

class RegionStats{
    init(name, area, perimeter){
        this.name = name;
        this.perimeter = perimeter;
        this.area = area;
    }
}

fn checkPerimeter(y,x, regionName){
    var perimeter = 0;

    if (y > 0 && board[y - 1][x] != regionName){
        perimeter += 1;
    }
    if (y == 0){
        perimeter += 1;
    }
    if (y == len(board) - 1){
        perimeter += 1;
    }
    if (y < len(board) - 1 && board[y + 1][x] != regionName){
        perimeter += 1;
    }
    if (x > 0 && board[y][x - 1] != regionName){
        perimeter += 1;
    }
    if (x < len(board[0]) - 1 && board[y][x + 1] != regionName){
        perimeter += 1;
    }
    if (x == 0){
        perimeter += 1;
    }
    if (x == len(board[0]) - 1){
        perimeter += 1;
    }

    return perimeter;
    # regions.compute(regionName, fn(k, v) {
    #     if (v == null){
    #         return RegionStats(1, perimeter);
    #     }
    #     v.area += 1;
    #     v.perimeter += perimeter;
    #     return v; 
    # })
}


fn getGroup(y, x, regionName, region){
    if (y < 0 || y >= len(board) || x < 0 || x >= len(board[0])){
        return;
    }
    if (checked[y][x]){
        return;
    }
    if (board[y][x] != regionName){
        return;
    }
    checked[y][x] = true;
    region.area += 1;
    region.perimeter += checkPerimeter(y, x, regionName);
    
    getGroup(y + 1, x, regionName, region);
    getGroup(y - 1, x, regionName, region);
    getGroup(y, x + 1, regionName, region);
    getGroup(y, x - 1, regionName, region);

    return region;
}

var ar = []

for (var y = 0; y < len(board); y += 1){
    for (var x = 0; x < len(board[0]); x += 1){
        if (!checked[y][x]){
            ar.append(getGroup(y, x, board[y][x], RegionStats(board[y][x], 0, 0)));
        }
        # checkPerimeter(y, x, board[y][x]);
    }
}
var tot = 0 
# regions.entries().foreach(fn (entry){
#     tot += entry.value.area * entry.value.perimeter;
#     print f"Region: {entry.key} Area: {entry.value.area} Perimeter: {entry.value.perimeter}"
# })



# print ar;

ar.foreach(fn (r){
    tot += r.area * r.perimeter;
    # print f"Region: {r.name} Area: {r.area} Perimeter: {r.perimeter}"
})

print f"tot: {tot}"