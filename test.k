from "lang" import __hierarchy__, Ansi

fn t(){
    print(__hierarchy__(Ansi))
}

t()

# ~
# class Pair {
#     init(first, second){
#         this.first = first;
#         this.second = second;
#     }
# var s = SQL("S")

# gc()

# var f = s.query("SELECT count(*) FROM users")
# s.close()

#     toStr(){
#         return f"({this.first}, {this.second})"#.f(this.first, this.second);
#     }
#     _eq_(other){
#         return this.first == other.first && this.second == other.second;
#     }
#     _hash_(){
#         return hash(this.first) + hash(this.second);
#     }
# }

# class GraphPair<Pair>{

#     init(){
#         super.init(1,fn()=>1);
#     }

#     toStr(){
#         return f"{super.toStr()} {this.second()}";
#     }
#   _lt_(other){
#     return this.second < other.second;
#   }
#   _gt_(other){
#     return this.second > other.second;
#   }
# }

# var p = GraphPair();
# print p

# class Graph {
#     init(){
#         this.adjList = HashMap();
#     }

#     addEdge(u, v){
#         this.adjList.compute(u, fn (k, val) => val ? val.append(v) : {v});
#     }

#     _get_(u){
#         return this.adjList[u];
#     }

#     _set_(u, v){
#         this.addEdge(u, v);
#     }

#     _hash_(){
#         return hash(this.adjList);
#     }

#     minDist(dist, sptSet){
#         var min = Math.INF;
#         var minIndex = -1;
#         dist.keys().foreach(fn (i) {
#             if (!sptSet.contains(i) && (dist[i]) < min){
#                 min = dist[i];
#                 minIndex = i;
#             }
#         });
#         return minIndex;
#     }

#     dijkstra(src){
#         var dist = HashMap();
#         this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
#         dist[src] = 0;
#         # print("dist: ${}".f(dist));
#         var sptSet = Set();
#         for (var i = 0; i < len(this.adjList); i+=1){
#             var u = this.minDist(dist, sptSet);
#             # print("u: ${}".f(u));
#             sptSet.add(u);
            
#             var adj = this.adjList.get(u, []);
#             print "adj: ${}".f(adj);
#             adj.foreach(fn (v) {
#                 # print "v: ${}".f(v);
#                 if (!sptSet.contains(v) && dist[u] != Math.INF && (dist[u] + 1) < dist.get(v, Math.INF)){
#                     dist[v] = dist[u] + 1;
#                 }
#             });
#         }
#         return dist;

#     }

#     toStr(){
#         var s = StringBuilder();
#         this.adjList.entries().foreach(fn (e) {
#             s.append(e[0]).append(" -> ").append(e[1]).append("\n");
#         });
#         return s.toStr();
#         # return this.adjList.toStr();
#     }
# }

# class WeightedDirectedGraph <Graph> {
#     init(){
#         this.adjList = HashMap();
#     }

#     addEdge(u, v, w){
#         this.adjList.compute(u, fn (k, val) => val ? val.append(GraphPair(v, w)) : {GraphPair(v, w)});
#     }

#     minDist(dist, sptSet){
#         var min = Math.INF;
#         var minIndex = -1;
#         var d = dist.keys();
#         for(var s = 0; s < len(d); s += 1){
#             var i = d[s];
#             if (!sptSet.contains(i) && (dist[i]) < min){
#                 min = dist[i];
#                 minIndex = i;
#             }
#         }
#         return minIndex;
#     }

#     dijkstra(src){
#         var dist = HashMap();
#         this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
#         dist[src] = 0;
#         # print("dist: ${}".f(dist));
#         # print "adjList: ${}".f(this.adjList);
#         var sptSet = [];
#         for (var i = 0; i < len(this.adjList.keys()); i+=1){
#             var u = this.minDist(dist, sptSet);
#             # print("u: ${}".f(u));
#             sptSet.append(u);
            
#             var adj = this.adjList.get(u, []);
#             # print "adj: ${}".f(adj);

            
#             adj.foreach(fn (vP) {
#                 var v = vP.name;
#                 var d = dist[u];
#                 # print "v: ${}".f(v);
#                 # print "dist[u]: ${}".f(d);
#                 # print "dist[v]: ${}".f(dist.get(v, Math.INF));
#                 # print "spSet: ${}".f(sptSet);
#                 if (!sptSet.contains(v) && d != Math.INF && (d + 1) < dist.get(v, Math.INF)){
#                     dist[v] = (dist[u] + 1);
#                 }
#             });
#         }
#         return dist;

#     }

#     shortestPath(src, dest){
#         var dist = this.dijkstra(src);
#         return dist[dest];
#     }



#     toStr(){
#         var s = StringBuilder();

#         var d = this.adjList.entries()
        
#         d.foreach(fn (e) {
#             s.append(f"{e[0]} -> {e[1]}\n")#).append(" -> ").append(e[1]).append('\n');
#         });
        
#         return s.toStr();
#         # return this.adjList.toStr();
#     }

# }


# var STDIN = 0
# var STDOUT = 1
# var STDERR = 2


# var g = WeightedDirectedGraph();
# g.addEdge("A", "B", 1);
# g.addEdge("A", "C", 1);
# g.addEdge("B", "D", 1);
# g.addEdge("C", "D", 1);
# g.addEdge("B", "A", 1);


# print g
# ~

# # fn inputMode(){
# #     if (s.key == 13 && s.mod == 2 && s.type == 1){
# #         x = -1;
# #         y += 1;
# #     }else if (s.key == 127 && s.mod == 2 && s.type == 1){
# #         x -= 1;
# #         tP(x, y, 0, 0, " ");
# #         tPres();
# #     }else{
# #         tP(x+=1,y,0,0,chr(s.ch));
# #         tPres();
# #     }
# #     s = tPoll();
# # }



# tI();
# # print ("TESTING DIJKSTRA");
# tP(0,0,0,0,"WELCOME!\n");
# tPres();
# var y = 1;
# var x = -1;
# var s = tPoll();
# while(s.mod != 2 || s.key != 4){
#     tP(0,y+=1,0,0,s.toStr());
#     # tP(0,y+=1,0,0,f"mod: {type(s.mod)}({s.mod}) key: {type(s.key)}({s.key}) type: {type(s.type)}({s.type})");
#     tPres();
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
# tClear();
# tC();
# Object.toStr;

# var g = Object();

# print g
# # print("ADADADASD")
# # tPres();
# # var s = tPoll();
# # tC();

# # import "stdlib"
# # import "lang"

# # var input = 
# # `3   4
# # 4   3
# # 2   5
# # 1   3
# # 3   9
# # 3   3`

# # print "PART 1"

# # input = with(File("./aoc2024/1"), fn (f) => f.read());

# # var left = []
# # var right = []


# # var lines = input.split("\n").foreach(fn (line) {
# #     var lineArr = line.split("   ");
# #     left.append(int(lineArr[0]));
# #     right.append(int(lineArr[1]));
# #     })
    
# # Arrays.mergeSort(left);
# # Arrays.mergeSort(right);

# # var dist = 0;
# # left.foreach(fn (index, l) {
# #     dist = dist + Math.abs(l - right[index]);
# # });

# # print f"dist: {dist}\n"


# # print "PART 2"

# # var rightMap = HashMap();

# # right.foreach(fn (num) {
# #     rightMap.compute(num, fn(k, v) => v ? v + 1 : 1);
# # });


# # var sim = 0;

# # left.foreach(fn(num){
# #   sim += (num * rightMap.get(num, 0));  
# # })

# # print f"similarity: {sim}"

# # import "stdlib"
# ~
# var input =
# `7 6 4 2 1
# 1 2 7 8 9
# 9 7 6 2 1
# 1 3 2 4 5
# 8 6 4 4 1
# 1 3 6 7 9`

# input = with(File("./2"), fn (f) => f.read());

# var lines = input.split("\n").map(fn(l) => l.split(" ").map(int));

# fn checkLine(line){
#     var isDec = line[0] > line[1];
#     for (var i = 0; i < len(line) - 1; i += 1){
#         if (line[i] == line[i + 1]){
#             return false;
#         }
#         if (isDec && (line[i] < line[i + 1])){
#             return false;
#         }
#         if (!isDec && (line[i] > line[i + 1])){
#             return false;
#         }
#         if (Math.abs(line[i] - line[i + 1]) > 3){ 
#             return false;
#         }

#     }
#     return true;
# }



# var s = 0;
# lines.foreach(fn (line) {
#     if (checkLine(line)){
#         s += 1;
#     }
# })

# # print f"valid: {s}"

# fn checkPair(i, x, direction){
#     if (i == x){
#         return false;
#     }
#     # -1 is ascending
#     # 1 is descending
#     if ((i - x) * direction < 0){
#         return false;
#     }

#     if (Math.abs(i - x) > 3){
#         return false;
#     }

#     return true;
# }

# class Direction{
#     ASC = -1;
#     DESC = 1;
#     INVALID = 0;
# }


# fn getDir(line){
#     var  inc = 0 
#     var dec = 0
#     var eq = 0
#     for (var i = 0; i < len(line) - 1; i += 1){
#         if (line[i] == line[i + 1]){
#             eq += 1;
#         }else
#         if (line[i] > line[i + 1]){
#             dec += 1;
#         }else
#         if (line[i] < line[i + 1]){
#             inc += 1;
#         }
#     } 
#     # print f"inc: {inc} dec: {dec} eq: {eq}";
#     if (eq > 1 || (inc > 1 && dec > 1)){
#         # print f"failed due to eq: {eq} inc: {inc} dec: {dec}";
#         return Direction.INVALID;
#     }
#     if (inc > dec){
#         return Direction.ASC;
#     }
#     else{
#         return Direction.DESC;
#     }
# }

# var numInvalid = 0;

# fn linechecker(line){
#     var i  = 0;
#     var encounteredErrorAt = -1;
#     var dir = getDir(line);
#     if (dir == Direction.INVALID){
#         numInvalid += 1;
#         return false;
#     }


# #    54 56 57 58
# # 
#     var c = 0;
#     for (var i = 0; i < len(line) - 1; i += 1){
#         # print f"prev: {prev} line[i]: {line[i]} c: {c}";
#         if (c > 1){
#             return false;
#         }
#         if (!checkPair(line[i], line[i + 1], dir)){
#             if (i >= 1 && checkPair(line[i - 1], line[i + 1], dir)){
#                 # print f"works at: {i} {line[i-1]} {line[i + 1]}";
#                 c += 1;
#                 continue
#             }
#             if ((i < len(line) - 2) && checkPair(line[i], line[i + 2], dir)){
#                 # d = true;
#                 # print f"works at: {i} {line[i]} {line[i + 2]}";
#                 # return false;
#                 c += 1;
#                 i+=1;
#                 continue
#             }
#             if (i < len(line) - 2 && checkPair(line[i + 1], line[i + 2], dir)){
#                 # print f"works at: {i} {line[i]} {line[i + 2]}";
#                 c += 1;
#                 continue
#             }
#             return false;
#         }
        
#     }
#     return true
# }

# var s = 0;

# # lines.foreach(fn (index, line) {
# #     if (linechecker(line)){
# #         # print f"\tI WORK: {index}"
# #         s += 1;
# #     }
# #  })

# # linechecker(lines[3])
# print f"valid: {s}"

# print f"invalid: {numInvalid}"



# print f"total: {len(lines)}"

# print f"t: {linechecker("1 3 6 5 7 10".split(" ").map(int))}"
# ~
# # rules has to only be decreasing or increasing and by no more than 2 and no less than 1
# ~
# import "stdlib"
# var input = 
# `xmul(2,4)%&mul[3,7]!@^do_not_mul(5,5)+mul(32,64]then(mul(11,8)mul(8,5))`

# input = `xmul(2,4)&mul[3,7]!^don't()_mul(5,5)+mul(32,64](mul(11,8)undo()?mul(8,5))`
# input = with(File("./3"), fn (f) => f.read());

# var re1 = `mul\([0-9]+,[0-9]+\)`
# var re2= `mul\([0-9]+,[0-9]+\)|do\(\)|don't\(\)`

# var muls = input.match(re2) #.match()
# # print f"muls: {muls}"

# var do = true;
# var tot = 0;

# muls.foreach(fn (match){
#     switch (match){
#         case ("do()")
#         {
#             do = true;
#             return;
#         }
#         case ("don't()")
#         {
#             do = false;
#             return;
#         }
#         default 
#         {
#             if (do){
#                 # print f"match: {match}"
#                 tot += reduce(match.match(`[0-9]+`).map(int), fn(a, b) => a * b, 1)

#                  #(nums[0] * nums[1]);
#             }
#         }
#     }
# })
# print f"tot: {tot}"
# # print f"tot: {reduce(muls.map(fn (m) =>reduce(m.match(`[0-9]+`).map(int), fn(a, b) => a * b, 1)), fn(a, b) => a + b, 0)}"


# actually, make it the map of nums that have to be b4 the current num
# {
# 47: [53, 13, 61, 29],
# 97: [13, 61, 47, 29, 75, 53]
# }

# and then loop through the page ordering and check if the 


# import "stdlib"

# var input = 
# `47|53
# 97|13
# 97|61
# 97|47
# 75|29
# 61|13
# 75|53
# 29|13
# 97|29
# 53|29
# 61|53
# 97|53
# 61|29
# 47|13
# 75|47
# 97|75
# 47|61
# 75|61
# 47|29
# 75|13
# 53|13

# 75,47,61,53,29
# 97,61,53,29,13
# 75,29,13
# 75,97,47,61,53
# 61,13,29
# 97,13,75,29,47`

# input = with(File("./5"), fn (f) => f.read());

# var sections= input.split("\n\n")
# var ruleArr = [100].map(fn(i) => [100]{false})
# var rules = sections[0].split("\n").map(fn (line) => line.split("|").map(int))
# rules.foreach(fn(rule) {
#  ruleArr[rule[0]][rule[1]] = true
# })
# var orders = sections[1].split("\n")#.map(fn (line) => line.split(",").map(int))

# fn sort(order){
#     var s = 
#     order
#     .split(",")
#     .map(int)
#     .sort(
#         fn(a,b) {
#             if(a == b) return 0;
#             return ruleArr[a][b]?-1:1;
#         })

#     return s#[len(s)//2].val;
# }


# var tot = 0;
# print sort("97,13,75,29,47")
# print sort("75,97,47,61,53")
# muls
# .map(fn (m) => 
#     m.match(`[0-9]+`)
#      .map(int)
#      .reduce(fn(a, b) => a * b, 1))
# .reduce(fn(a, b) => a + b, 0)
# # orders.foreach(fn(i, order){
# #     var isValid = true;
# #     for (var j = 0; j < len(rules); j += 1){
# #         if (order.match(`${}\S*${}`.f(rules[j][1], rules[j][0]))){
# #             isValid = false;
# #             break;
# #         }
# #     }
# #     if (!isValid){
# #         tot += sort(order)
# #     }
# # })
# # print f"tot {tot}"




# import "lang"

# var input =
# `MMMSXXMASM
# MSAMXMSMSA
# AMXSXMAAMM
# MSAMASMSMX
# XMASAMXAMM
# XXAMMXXAMA
# SMSMSASXSS
# SAXAMASAAA
# MAMMMXMMMM
# MXMXAXMASX`




# input = with(File("./4"), fn (f) => f.read());

# var board = input.split("\n").map(list)
# # check every direction in star pattern
# var XMAS = "XMAS"
# var MAS = "MAS"

# var dirs = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}



# var totp1 = 0 
# var totp2 = 0


# fn checkDir(board, x, y, dx, dy){
#     var index = 1;
#     for (var i = 1; i < 4; i += 1){
#         var nx = x + (dx * i);
#         var ny = y + (dy * i);
#         if (nx < 0 || nx >= len(board[0]) || ny < 0 || ny >= len(board)){
#             return 0;
#         }
#         if (board[ny][nx] != XMAS[index]){
#             return 0;
#         }
#         index += 1;
#         if (index == 4){
#             break;
#         }
#     }
#     return 1;
# }

# fn checkForXMAS(board, x, y){
#     var xmas = 0;
#     var index = 1;


#     dirs.foreach(fn(dir){
#         var dx = dir[0];
#         var dy = dir[1];
#         xmas += checkDir(board, x, y, dx, dy);
#     })
#     return xmas;

# }

# fn checkForCrossMAS(board, x, y){

#     if (x == 0 || y == 0 || x == len(board[0]) - 1 || y == len(board) - 1){
#         return 0;
#     }

#     if (   (board[y-1][x-1] != "M" && board[y+1][x+1] != "M")
#         || (board[y-1][x-1] != "S" && board[y+1][x+1] != "S")
#         || (board[y-1][x-1] == "S" && board[y+1][x+1] != "M")
#         || (board[y-1][x-1] == "M" && board[y+1][x+1] != "S")){
#         return 0;
#     }
    
#     if (   (board[y-1][x+1] != "S" && board[y+1][x-1] != "S")
#         || (board[y-1][x+1] != "M" && board[y+1][x-1] != "M")
#         || (board[y-1][x+1] == "S" && board[y+1][x-1] != "M")
#         || (board[y-1][x+1] == "M" && board[y+1][x-1] != "S")){
#         return 0;
#     }

#     return 1;
# }


# for (var y = 0; y < len(board); y += 1){
#     var row = board[y];
#     for (var x = 0; x < len(row); x += 1){
#         if (row[x] == "X"){
#             totp1 += checkForXMAS(board, x, y)
#         }
#         else 
#         if (row[x] == "A"){
#             totp2 += checkForCrossMAS(board, x, y)
#         }
#     }
# }
# print f"tot 4 p1: {totp1}\ntot 4 p2: {totp2}"
# ~


# ~
# var s = {5,4,3,2,1}
# var g = fn(a,b) { if (a == b) return 0; return b>a?1:-1}
# s.sort(g)


# import "lang"

# var input =
# `....#.....
# .........#
# ..........
# ..#.......
# .......#..
# ..........
# .#..^.....
# ........#.
# #.........
# ......#...`

# var input = 
# `...........#.....#......
# ...................#....
# ...#.....##.............
# ......................#.
# ..................#.....
# ..#.....................
# ....................#...
# ........................
# .#........^.............
# ..........#..........#..
# ..#.....#..........#....
# ........#.....#..#......`

# input = with(File("./6"), fn (f) => f.read());

# var board = input.split("\n").map(list)

# var index = input.find("^")
# var x = index % (len(board[0]) + 1)
# var y = index // (len(board[0]) + 1)

# print f"x: {x} y: {y}"#" len: x: {len(board[0])} y: {len(board)}"

# class Direction {
#     UP = {-1,0};
#     RIGHT = {0,1};
#     DOWN = {1,0};
#     LEFT = {0,-1};

#     turnRight(dir){
#         switch(dir){
#             case (Direction.UP){
#                 return Direction.RIGHT;
#             }
#             case (Direction.RIGHT){
#                 return Direction.DOWN;
#             }
#             case (Direction.DOWN){
#                 return Direction.LEFT;
#             }
#             case (Direction.LEFT){
#                 return Direction.UP;
#             }
#         }
#     }

# }

# fn inBounds(x1, y1){
#     # print f"x1: {x1} y1: {y1} len: x: {len(board[0])} y: {len(board)}"
#     return x1 >= 0 && x1 < len(board[0]) && y1 >= 0 && y1 < len(board);
# }

# class Gaurd {
#     init(x, y, dir){
#         this.x = x;
#         this.y = y;
#         this.dir = dir;
#         # this.startDir = dir[:];
#         this.looped = false;
#         this.oob= false;
#         this.locations = {{y, x}};
#         this.path = {{y, x, dir}};
#         this.extraObsticals = []
#     }

#     move(){
        
#         var nx = this.x + this.dir[1];
#         var ny = this.y + this.dir[0];
#         if (inBounds(nx, ny)){
#             # print f"c: {board[ny][nx]} nx: {nx} ny: {ny}"
#             if (board[ny][nx] == "#" ){

#                 var prev = this.dir;
#                 this.dir = Direction.turnRight(this.dir);
#                     nx = this.x + this.dir[1];
#                     ny = this.y + this.dir[0];
#                 while(board[ny][nx] == "#" && this.dir != prev){
#                     this.dir = Direction.turnRight(this.dir);
#                     nx = this.x + this.dir[1];
#                     ny = this.y + this.dir[0];
#                 }
#                 if(this.dir == prev){
#                     this.oob = true;
#                     return;
#                 }
                
#                 # print f"new dir: {this.dir} at this.extraObsticals: {this.extraObsticals}"
#             }
  
#             this.x += this.dir[1];
#             this.y += this.dir[0];
#             if (this.path.contains({this.y, this.x, this.dir})){
#                 this.looped = true;
#             }
            
            
#             if (!this.locations.contains({this.y, this.x})){
#                 this.locations.append({this.y, this.x});
#             }
            
#         }else{
#             this.oob = true;
#         }
#         this.path.append({this.y, this.x, this.dir});
#     }

#     turnRight(){
#         this.dir = Direction.turnRight(this.dir);
#     }

#     addObstical(pathPoint){
#         var nx = pathPoint[1];
#         var ny = pathPoint[0];

#         if (inBounds(nx, ny)){
#             if(nx != x || ny != y ){
#                 this.extraObsticals = {ny, nx};
#                 board[ny][nx] = "#";
#             }else{
#                 return false;
#             }
#         }else{
#             this.oob = true;
#         }
#         return true;
#     }

#     resetBoard(){
#         if(this.extraObsticals){
#             board[this.extraObsticals[0]][this.extraObsticals[1]] = ".";
#         }
#     }

# }

# var g = Gaurd(x, y, Direction.UP)

# while (!g.looped && !g.oob){
#     g.move();
# }

# print len(g.locations)
# # print g

# var looped = 0
# var oob = 0
# var r = 0
# var obs=  []
# var failed = HashMap()
# # var edited = false
# for (var i = 0; i < len(g.path) - 1; i += 1){

#     var g2 = Gaurd(g.path[i][1], g.path[i][0], g.path[i][2]);
#     # if (){
# # print f"adding obstical at {g2.x}, {g2.y}"
#     if (failed.containsKey(g.path[i+1][:2]) || !g2.addObstical(g.path[i+1])){
#         continue;
#     }

#     while (!g2.looped && !g2.oob){
#         g2.move();
#     }

#     if (g2.looped){
#         # print(f"LOOPED: {g2.path}")
#         if (!obs.contains(g2.extraObsticals)){
#             obs.append(g2.extraObsticals)
#         }
#         looped += 1;
#         failed[g2.extraObsticals] = true;
#         # sleep(5)
#     }

#     if (g2.oob){
#         failed[g2.extraObsticals] = false;
#         oob += 1;
#     }
#     g2.resetBoard();
#     if (i % 500 == 0){
#         print f"i: {i} score: {len(obs)} | {looped} oob: {oob}"
#     }

# } 
# # import "stdlib"
# # print reduce(failed.entries(), fn(a, b) {
# #     if (b[1]){
# #         print "b: ${}".f(b)
# #         return a + 1;
# #     }
# #     return a;
# # }, 0)
# print f"{len(g.path)} score: {len(obs)} {looped} oob: {oob}"
# # print f"obs: {obs}"



# import "lang"

# var input =
# `190: 10 19
# 3267: 81 40 27
# 83: 17 5
# 156: 15 6
# 7290: 6 8 6 15
# 161011: 16 10 13
# 192: 17 8 14
# 21037: 9 7 18 13
# 292: 11 6 16 20`

# input = with(File("./t7"), fn (f) => f.read());

# var lines = input.split("\n").map(fn(l) => l.split(": ")).map(fn(l) {
#     var nums = l[1].split(" ").map(int);
#     return {int(l[0]), nums};
# })

# # print lines 


# var OP_ADD = "+"
# var OP_MUL = "*"
# var OP_CONCAT = "||"

# var OPS = {OP_ADD, OP_MUL, OP_CONCAT}

# var combCache = HashMap()

# # get all permutations of nums and operators


# fn product(it, repeat){
#     fn helper(rest){
#         if (!rest){
#             return {{}};
#         }
#         var first = rest[0]
#         var rest_ = helper(rest[1:])
#         var ret = []
#         first.foreach(fn(item){
#             rest_.foreach(fn(sub){
#                 ret.append({item}.extend(sub))
#             })
#         })
#         return ret;
#     }

#     fn mul(it, repeat){
#         if (repeat == 1){
#             return {it};
#         } 
#         return [repeat]{it}
#     }
    
#     return helper(mul(it, repeat))

# }


# fn permutations(nums ){

#     var nPos = len(OPS)**(len(nums) - 1)
#     var ret = [nPos].map(fn(i) => [])

#     combCache.computeIfAbsent(nPos, fn (_) => product(OPS, len(nums) - 1))
#     var combination = combCache[nPos]
#     # print f"combination: {combination}"
#     var combIndex = 0;
#     for (var i = 0; i < len(nums) - 1; i+=1){
#         for (var j = 0; j < nPos; j+=1){
#             ret[j].append(nums[i]);
#             ret[j].append(combination[j][i]);   
#             # combIndex += 1;
#         }
#         # for (var c = 0; c < len(combination[combIndex]); c+=1){
#         #     ret[c].append(combination[combIndex][c]);
#         # }
#         # for (var j = 0; j < len(nums) -1 ; j+=1){
#         #     print f"j: {j} combIndex: {combIndex}"
#         #     print f"combination[combIndex][j]: {combination[combIndex][j]}"
#         #     ret[j].append(combination[combIndex][j]);
#         # }

#     }

#     for (var j = 0; j < nPos; j+=1){
#         ret[j].append(nums[-1]);
#     }

#     return ret 
# }


# var r = 0
# lines.foreach(fn(l){
#     # var prod = product(OPS, len(l[1]) - 1)
#     var tot = l[0]
#     var fund = false;
#     permutations(l[1]).foreach(fn(p){
#         # print p
#         var sum = p[0]
#         # go through and perform the concatenation
        
#         var i = 1;
#         while (i < len(p)){            
#             if (p[i] == OP_ADD){
#                 sum += p[i + 1];
#             }else if (p[i] == OP_MUL){
#                 sum *= p[i + 1];
#             }else if (p[i] == OP_CONCAT){
#             # print f"BEFORE: {p}"

#                 sum = int("${}${}".f(sum, p[i + 1]));

#                 p.pop(i + 1);
#                 p.pop(i);
#                 p[i - 1] = sum
#                 i -= 2;
#             }
#             # print f"UPDATE: {p} {sum} {i} {p[i]}"
#             i += 1;
#         }
#         # print f"sum: {sum} I: {i}"s
        

#         if (sum == tot && !fund){
#             fund = true;
#             r += tot;
#             # print f"\t\tsum: {sum} p: {p}"
#         }
#     })

# })

# print f"r: {r}"


# # print permutations({10, 19}, OPS)
# # print permutations({10, 19, 9}, OPS)
# # print permutations({10, 19, 9,3}, OPS)


# # lines.foreach(fn(l){
# #     if ()
# # })












# import "lang"


# var input = 
# `............
# ........0...
# .....0......
# .......0....
# ....0.......
# ......A.....
# ............
# ............
# ........A...
# .........A..
# ............
# ............`


# # input =
# # `..........
# # ..........
# # ..........
# # ....a.....
# # ........a.
# # .....a....
# # ..........
# # ..........
# # ..........
# # ..........`

# input = with(File("./t8"), fn (f) => f.read());

# var board = input.split("\n").map(list)


# var freqPoints = HashMap()
# var ch = []
# var hashes = []
# var numChecked = 0
# board.foreach(fn(y, row){
#     row.foreach(fn(x, char){
#         # print f"char: {char}"
#         if (char == '.'){
#             return;
#         }
#         # if (!ch.contains(char)){
#         #     ch.append(char);
#         #     hashes.append(hash(char))#{hash(char), char})
            
#         # }
#         # var ss= freqPoints.get(char)
#         # print f"c: {char} ss: {ss}"
#         # if (!ss){
#         #     freqPoints[char] = {{y, x}}
#         # }
#         # freqPoints[char].append({y, x})
#         freqPoints.compute(char, fn(k, v) => v ? v.append({y, x}) : {{y, x}});
#     })
# })

# # print freqPoints
# print f"numChecked: {len(freqPoints.keys())}"
 
# var entries = freqPoints.entries()




# fn abs(a){
#     return a < 0 ? -a : a;
# }

# fn manhattenDist (p1, p2){
#     return {p2[0] - p1[0], p2[1] - p1[1]};
# }


# fn isInBounds(x, y){
#     return x >= 0 && x < len(board[0]) && y >= 0 && y < len(board);
# }

# fn isPIn(point, offset){
#     return isInBounds(point[1] + offset[1], point[0] + offset[0]) ;
# }

# fn isnPIn(point, offset){
#     return isInBounds(point[1] - offset[1], point[0] - offset[0]) #&& {point[0] - offset[0],point[1] - offset[1]} != p;
# }


# var hash1 = HashMap()

# entries.foreach(fn(entry){
#     if (len(entry.value) < 2){
#         return;
#     }
#     for (var i = 0; i < len(entry.value); i += 1){
#         var p1 = entry.value[i];
#         for(var j = i + 1; j < len(entry.value); j += 1){
#             var p2 = entry.value[j];
#             var dist = manhattenDist(p1, p2);
#             var dy = p2[0] - p1[0];
#             var dx = p2[1] - p1[1];

#             var anti = {p1[0] - dy, p1[1] - dx}
#             if (isInBounds(anti[1], anti[0])){
#                 hash1[anti] = true;
#             }


#             anti = {p2[0] + dy, p2[1] + dx}
#             if (isInBounds(anti[1], anti[0])){
#                 hash1[anti] = true;
#             }
#             # print f"{entry.key}: dist: {dist} from {p1} to {p2}"
#             # if (isPIn(p1, dist, p2)){
#             #     # print f"{entry.key}: ANTINODE: {p1[0] + dist[0]} {p1[1] + dist[1]}"
#             #     hash[{p1[0] + dist[0], p1[1] + dist[1]}] = true;
#             # }
#             # if(isnPIn(p1, dist)){
#             #     # print f"{entry.key}: ANTINODE: {p1[0] - dist[0]} {p1[1] - dist[1]}"
#             #     hash1[{p1[0] - dist[0], p1[1] - dist[1]}] = true;
#             #     f.add({p1[0] - dist[0], p1[1] - dist[1]})
#             #     gg.append({p1[0] - dist[0], p1[1] - dist[1]})
#             # }

#             # if (isPIn(p2, dist)){   
#             #     # print f"{entry.key}: ANTINODE: {p2[0] + dist[0]} {p2[1] + dist[1]}"
#             #     hash1[{p2[0] + dist[0], p2[1] + dist[1]}] = true;
#             #     f.add({p2[0] + dist[0], p2[1] + dist[1]})
#             #     gg.append({p2[0] + dist[0], p2[1] + dist[1]})
#             # }
            
#             # if(isnPIn(p2, dist, p1)){
#             #     # print f"{entry.key}: ANTINODE: {p2[0] - dist[0]} {p2[1] - dist[1]}"
#             #     hash[{p2[0] - dist[0], p2[1] - dist[1]}] = true;
#             # }
#         }
#     }
# })

# print len(hash1.keys())


# import "lang"

# var input = list("2333133121414131402").map(int)

# input = list(with(File("./t9"), fn (f) => f.read())).map(int)

# var fileId=-1
# var fileSize = HashMap()
# var mem = []

# input.foreach(fn (i, c){
#     if ((i % 2) == 0){
#         fileId += 1;
#         fileSize[fileId] = c;
#     }
#     for (var j = 0; j < c; j += 1){
#         mem.append((i % 2) == 0 ? fileId : null);
#     }
# })

# # print 

# var mem2 = mem
# # print 
# for (var i = len(mem2) - 1; i >= 0; i -= 1){
#     if (mem2[i] != null){
#         var size = fileSize[mem2[i]];
#         print f"i: {i} mem2[i]: {mem2[i]} size: {size}"

#         for (var j = 0;j < i;j+=1){
#             if (mem2[j] == null){
#                 var end = j;
#                 while (end < len(mem2) && mem2[end] == null){
#                     end += 1;
#                 }
#                 if (end - j >= size){
#                     # print f"\tFOUND END: {end} j: {j} "
#                     for (var k = 0; k < size; k += 1){
#                         mem2[j + k] = mem2[i];
#                     }
#                     # print f"mem2: {mem2}"
#                     for (var k = 0; k < size; k += 1){
#                         mem2[i - k] = null;
#                     }
                    
#                     break;
#                 }
#             }
#         }
#     }
#     if (i % 1000 == 0){
#         print f"processed i: { i } / { len(mem2) }"
#     }
# }

# # print mem2



# # for (var i = 0; i<len(mem2); i+=1){
# #     var end = i;
# #     if (mem2[i] == null){
# #         while(end < len(mem2) && mem2[end] == null){
# #             end += 1;
# #         }
# #         if (end - i > 1){
# #             var size = end - i;
# #             if (availFree.containsKey(size)){
# #                 availFree[size].append(i);
# #             }else{
# #                 availFree[size] = {i};
# #             }
# #         }
# #     }
# # }

# var tot2 = 0

# mem2.foreach(fn(i, m){
#     if (m != null){
#         tot2 += m * i;
#     }
# })

# print tot2

# # print mem

# # # PART 1
# # for (var i = 0; i < len(mem); i += 1){
# #     if (mem[i] == null){
# #         var j = len(mem) - 1;
# #         while (j > i && mem[j] == null){
# #             j -= 1;
# #         }
# #         if (j > i){
# #             mem[i] = mem[j];
# #             mem[j] = null;
# #         }
# #     }
# #     if (i % 1000 == 0){
# #         print f"processed i: {i}"
# #     }
# # }




# # # print mem
# # print "files: " + fileSize
# # print "frees: " + availFree

# # var tot = 0

# # mem.foreach(fn(i, m){
# #     if (m != null){
# #         tot += m * i;
# #     }
# # })

# # print tot 



# import "lang"

# var input = 
# `89010123
# 78121874
# 87430965
# 96549874
# 45678903
# 32019012
# 01329801
# 10456732`

# input = with(File("./t10"), fn (f) => f.read());

# var board = input.split("\n").map(fn (r) => list(r).map(int))

# # print f"board: {board}"


# fn isThereATo9(y,x, setOfPoints){

#     var pos = board[y][x];
#     # print f"{id} Checking: pos: {pos} y: {y} x: {x}"
#     if (y < 0 || y >= len(board) || x < 0 || x >= len(board[0])){
#         return 0 ;
#     }
#     if (pos == 9){
#         # print "FOUND 9"
#         setOfPoints.put({y, x}, true);
#         return 1;
#     }
#     var sum = 0

#     if (y < len(board) - 1 && pos + 1 == board[y + 1][x]){
#         sum += isThereATo9( y + 1, x, setOfPoints);
#     }
#     if (y > 0 && pos + 1 == board[y - 1][x]){
#         sum += isThereATo9( y - 1, x, setOfPoints);
#     }
#     if (x < len(board[0]) - 1 && pos + 1 == board[y][x+1]){
#         sum += isThereATo9( y, x + 1, setOfPoints);
#     }
#     if (x > 0 && pos + 1 == board[y][x - 1]){
#         sum += isThereATo9( y, x - 1, setOfPoints);
#     }
#     # print_error("damn")
#     return sum;
# }




# var setOfPoints = HashMap()
# var sum = 0;
# var p2 = 0;
# board.foreach(fn (y, row){
#     row.foreach(fn (x, pos){
#         setOfPoints.clear()
#         if (pos == 0){
#             p2 += isThereATo9(y, x, setOfPoints);
#         }
#         sum += len(setOfPoints);
#     })
# })

# print f"setOfPoints: {sum}"
# print f"p2: {p2}"




# import "lang"


# var input = 
# `RRRRIICCFF
# RRRRIICCCF
# VVRRRCCFFF
# VVRCCCJFFF
# VVVVCJJCFE
# VVIVCCJJEE
# VVIIICJJEE
# MIIIIIJJEE
# MIIISIJEEE
# MMMISSJEEE`

# # input = 
# # `AAAA
# # BBCD
# # BBCC
# # EEEC`


# # input = 
# # `OOOOO
# # OXOXO
# # OOOOO
# # OXOXO
# # OOOOO`

# input = with(File("./t12"), fn (f) => f.read());

# var board = input.split("\n").map(list)
# var checked = [len(board)].map(fn(_) => [len(board[0])]{false})



# var regions = HashMap()

# class RegionStats{
#     init(name, area, perimeter){
#         this.name = name;
#         this.perimeter = perimeter;
#         this.area = area;
#     }
# }

# fn checkPerimeter(y,x, regionName){
#     var perimeter = 0;

#     if (y > 0 && board[y - 1][x] != regionName){
#         perimeter += 1;
#     }
#     if (y == 0){
#         perimeter += 1;
#     }
#     if (y == len(board) - 1){
#         perimeter += 1;
#     }
#     if (y < len(board) - 1 && board[y + 1][x] != regionName){
#         perimeter += 1;
#     }
#     if (x > 0 && board[y][x - 1] != regionName){
#         perimeter += 1;
#     }
#     if (x < len(board[0]) - 1 && board[y][x + 1] != regionName){
#         perimeter += 1;
#     }
#     if (x == 0){
#         perimeter += 1;
#     }
#     if (x == len(board[0]) - 1){
#         perimeter += 1;
#     }

#     return perimeter;
#     # regions.compute(regionName, fn(k, v) {
#     #     if (v == null){
#     #         return RegionStats(1, perimeter);
#     #     }
#     #     v.area += 1;
#     #     v.perimeter += perimeter;
#     #     return v; 
#     # })
# }


# fn getGroup(y, x, regionName, region){
#     if (y < 0 || y >= len(board) || x < 0 || x >= len(board[0])){
#         return;
#     }
#     if (checked[y][x]){
#         return;
#     }
#     if (board[y][x] != regionName){
#         return;
#     }
#     checked[y][x] = true;
#     region.area += 1;
#     region.perimeter += checkPerimeter(y, x, regionName);
    
#     getGroup(y + 1, x, regionName, region);
#     getGroup(y - 1, x, regionName, region);
#     getGroup(y, x + 1, regionName, region);
#     getGroup(y, x - 1, regionName, region);

#     return region;
# }

# var ar = []

# for (var y = 0; y < len(board); y += 1){
#     for (var x = 0; x < len(board[0]); x += 1){
#         if (!checked[y][x]){
#             ar.append(getGroup(y, x, board[y][x], RegionStats(board[y][x], 0, 0)));
#         }
#         # checkPerimeter(y, x, board[y][x]);
#     }
# }
# var tot = 0 
# # regions.entries().foreach(fn (entry){
# #     tot += entry.value.area * entry.value.perimeter;
# #     print f"Region: {entry.key} Area: {entry.value.area} Perimeter: {entry.value.perimeter}"
# # })



# # print ar;

# ar.foreach(fn (r){
#     tot += r.area * r.perimeter;
#     # print f"Region: {r.name} Area: {r.area} Perimeter: {r.perimeter}"
# })

# print f"tot: {tot}"
# var input = 
# `p=0,4 v=3,-3
# p=6,3 v=-1,-3
# p=10,3 v=-1,2
# p=2,0 v=2,-1
# p=0,0 v=1,3
# p=3,0 v=-2,-2
# p=7,6 v=-1,-3
# p=3,0 v=-1,-2
# p=9,3 v=2,3
# p=7,3 v=-1,2
# p=2,4 v=2,-3
# p=9,5 v=-3,-3`

# input = with(File("./t14"), fn (f) => f.read());

# var re = `p=([0-9]+),([0-9]+) v=(-?[0-9]+),(-?[0-9]+)`

# var pAndVs = input.findall(re).map(fn (m) => m.map(int))

# # p = v * t + p0

# # var robots = HashMap()

# var boardX = 101
# var boardY = 103

# # var s = fn  (row){
# #             out.write("".join(row));
# #         }


# var out = File("tree.txt", "w")


# var board = [boardY].map(fn (_) => [boardX]{"."})

# var startTime = 6875
# var endTime = 6876 


# for (var i = 0; i < len(pAndVs); i += 1){
#     var p0x = pAndVs[i][0];
#     var p0y = pAndVs[i][1];
#     var vx = pAndVs[i][2];
#     var vy = pAndVs[i][3];
#     pAndVs[i][0] = (vx * startTime + p0x) % boardX;
#     pAndVs[i][1] = (vy * startTime + p0y) % boardY; 
    
# }   



# for (var j = startTime; j < endTime; j += 1){
    
#     for (var i = 0; i < len(pAndVs); i += 1){
#         var p0x = pAndVs[i][0];
#         var p0y = pAndVs[i][1];
#         var vx = pAndVs[i][2];
#         var vy = pAndVs[i][3];
  
#         # var px = p0x#$(vx * 100 + p0x) % boardX;
#         # var py = p0y#(vy * 100 + p0y) % boardY; 
#         # print f"p: {p0x}, {p0y} v: {vx}, {vy}"
#         # # print "After 100 seconds:"
#         var px = (vx + p0x);
#         if (px < 0){
#             px = boardX + px;
#         }else if (px >= boardX){
#             px = px % boardX;
#         }

#         var py = (p0y + vy) 
#         if (py < 0){
#             py = boardY + py;
#         }else if (py >= boardY){
#             py = py % boardY;
#         }
#         board[p0y][p0x] = ".";
#         board[py][px] = "#";
#         pAndVs[i][0] = px;
#         pAndVs[i][1] = py;

#     }
#     board.foreach(fn  (row){
#         out.write("".join(row) + "\n");
#     })  
#     out.write(f"{j}\n\n");  

#     if (j % 20 == 0){
#         print f"j: {j}/{endTime}"
#     }
    
# }
#     # var px = (vx * 100 + p0x) % boardX;
#     # var py = (vy * 100 + p0y) % boardY; 
#     # print f"p: {px}, {py}"
#     # print f"p: {px}, {py}\n"
#     # board[py][px] = board[py][px] + 1; # P1
#     # robots[i] = {p, v, p0, v0}
# out.close()

# var midx = boardX // 2
# var midy = boardY // 2

# var quadrents = {0,0,0,0}

# # for (var y = 0; y < boardY; y += 1){
# #     for (var x = 0; x < boardX; x += 1){
# #         if (x < midx && y < midy){
# #             quadrents[0] = quadrents[0] + board[y][x];
# #         }else if (x > midx && y < midy){
# #             quadrents[1] = quadrents[1] + board[y][x];
# #         }else if (x < midx && y > midy){
# #             quadrents[2] = quadrents[2] + board[y][x];
# #         }else if (x > midx && y > midy){
# #             quadrents[3] = quadrents[3] + board[y][x];
# #         }
# #     }
# # }    

# print f"quadrents: {quadrents}"

# var tot = 1
# quadrents.foreach(fn (q){
#     tot *= q;
# })

# print f"tot: {tot}"

# var input = 
# `##########
# #..O..O.O#
# #......O.#
# #.OO..O.O#
# #..O@..O.#
# #O#..O...#
# #O..O..O.#
# #.OO.O.OO#
# #....O...#
# ##########

# <vv>^<v^>v>^vv^v>v<>v^v<v<^vv<<<^><<><>>v<vvv<>^v^>^<<<><<v<<<v^vv^v>^
# vvv<<^>^v^^><<>>><>^<<><^vv^^<>vvv<>><^^v>^>vv<>v<<<<v<^v>^<^^>>>^<v<v
# ><>vv>v^v^<>><>>>><^^>vv>v<^^^>>v^v^<^^>v^^>v^<^v>v<>>v^v^<v>v^^<^^vv<
# <<v<^>>^^^^>>>v^<>vvv^><v<<<>^^^vv^<vvv>^>v<^^^^v<>^>vvvv><>>v^<<^^^^^
# ^><^><>>><>^^<<^^v>>><^<v>^<vv>>v>>>^v><>^v><<<<v>>v<v<v>vvv>^<><<>^><
# ^>><>^v<><^vvv<^^<><v<<<<<><^v<<<><<<^^<v<^^^><^>>^<v^><<<^>>^v<v^v<v^
# >^>>^v>vv>^<<^v<>><<><<v<<v><>v<^vv<<<>^^v^>^^>>><<^v>>v^v><^^>>^<>vv^
# <><^^>^^^<><vvvvv^v<v<<>^v<v>v<<^><<><<><<<^^<<<^<<>><<><^^^>^^<>^>v<>
# ^^>vv<^v^v<vv>^<><v<^v>^^^>>>^^vvv^>vvv<>>>^<^>>>>>^<<^v>^vvv<>^<><<v>
# v^^>>><<^^<>>^v^<v^vv<>v^<<>^<^v^v><^<<<><<^<v><v<>vv>>v><v^<vv<>v^<<^`

# # input = 
# # `#######
# # #...#.#
# # #.....#
# # #..OO@#
# # #..O..#
# # #.....#
# # #######

# # <vv<<^^<<^^`
# # vv<<^^<<^^`

# input = with(File("./t15"), fn (f) => f.read());

# var boardNMoves = input.split("\n\n")
# # var robotIndex = boardNMoves[0].find("@")
# var board = boardNMoves[0].split("\n").map(list)
# var robotX;# = robotIndex % (len(board[0]))
# var robotY;# = robotIndex // len(board)

# board.foreach(fn (y, row){
#     if (row.contains("@")){
#         robotX = row.indexOf("@");
#         robotY = y;
#         return false;
#     }
# })
# var moves = list(boardNMoves[1]).filter(fn (c) => c == "\n")

# var movesMap = HashMap()
# movesMap["^"] = {-1,0}
# movesMap["v"] = {1,0}
# movesMap["<"] = {0,-1}
# movesMap[">"] = {0,1}


# var scaleUpRules = HashMap()
# scaleUpRules["#"] = {"#", "#"}
# scaleUpRules["O"] = {"[", "]"}
# scaleUpRules["."] = {".", "."}
# scaleUpRules["@"] = {"@", "."}


# fn scaleUp(b){
#     var bigBoard = [len(b)].map(fn (_) => [])

#     for (var y = 0; y < len(b); y += 1){
#         for (var x = 0; x < len(b[0]); x += 1){
#             bigBoard[y].extend(scaleUpRules[b[y][x]]);
#         }
#     }

#     return bigBoard;
# }


# var bigBoard = scaleUp(board)

# # bigBoard.foreach(fn (row){
# #     print "".join(row);
# # })

# # exit(0) 

# var rX = robotX * 2
# var rY = robotY

# # board.foreach(fn (row){
# #     print " ".join(row)
# # })

# # print f"robotX: {robotX} robotY: {robotY} board: {board[robotY][robotX]}"

# # print f"moves: {moves}"


# fn inBounds(x, y, board){
#     return x >= 0 && x < len(board[0]) && y >= 0 && y < len(board);
# }

# fn pushOinDir(y, x, dx, dy){
#     var x0 = x;
#     var y0 = y;
#     while (inBounds(x,y,board) && board[y][x] == "O"){
#         x += dx;
#         y += dy;
#     }
#     board[y0][x0] = ".";
#     board[y][x] = "O";
    
# }

# fn canPushOinDir(y, x, dx, dy){
#     while (inBounds(x,y, board) && board[y][x] == "O"){
#         x += dx;
#         y += dy;
#     }
#     return board[y][x] == ".";
# }


# var logInterval = len(moves) // 10;


# fn handleP1(i){
#     var move = moves[i];
#     var moveX = movesMap[move][1];
#     var moveY = movesMap[move][0];
#     var newX = robotX + moveX;
#     var newY = robotY + moveY;
#     if (newX < 0 || newX >= len(board[0]) || newY < 0 || newY >= len(board)){
#         return;
#     }
#     if (board[newY][newX] == "#"){
#         return;
#     }
#     if (board[newY][newX] == "O"){
#         if (canPushOinDir(newY, newX, moveX, moveY)){
#             pushOinDir(newY, newX, moveX, moveY);
#         }else{
#             return;
#         }
#     }
#     board[robotY][robotX] = ".";

#     robotX = newX;
#     robotY = newY;
#     board[robotY][robotX] = "@";
# }

# fn rec(x,y,dx,dy, memo){
    
#     if (memo.containsKey({y, x, dx, dy})){
#         return memo[{y, x, dx, dy}];
#     }
#     if (y < 0 || y >= len(bigBoard) || x < 0 || x >= len(bigBoard[0])){
#         return memo[{y, x, dx, dy}] = false;
#     }
#     if (bigBoard[y][x] == "#"){
#         return memo[{y, x, dx, dy}] = false;
#     }
#     if (bigBoard[y][x] == "]"){
#         return memo[{y, x, dx, dy}] = rec(x - 1, y + dy, dx, dy, memo) && rec(x, y + dy, dx, dy, memo); 
#     }
#     if (bigBoard[y][x] == "["){
#         return memo[{y, x, dx, dy}] = rec(x + 1, y + dy, dx, dy, memo) && rec(x, y + dy, dx, dy, memo); 
#     }
#     return memo[{y, x, dx, dy}] = true;

# }

# fn canPushBoxinDir(y, x, dx, dy){
#     var x0 = x;
#     var y0 = y;

#     if (dy == 0){
#         while (inBounds(x,y, bigBoard) && (bigBoard[y][x] == "[" || bigBoard[y][x] == "]")){
#             x += dx;
#         }
#         return bigBoard[y][x] == ".";
#     }else{
#         var memo = HashMap()
#         var r = rec(x, y, dx, dy, memo);
#         # print f"r: {r} memo: {memo}"
#         return r;
#     }
# }

# fn recMove(x,y,dy){
#     # print f"recMove: {x} {y} {dy} {bigBoard[y][x]}"s
#     var x0 = x;
#     var y0 = y;
#     if (bigBoard[y][x] == "."){
#         return;
#     }
#     var isLeft = bigBoard[y][x] == "[";
#     var otherX = x + (isLeft ? 1 : -1);

#     var next = bigBoard[y + dy][x];

#     recMove(x, y + dy, dy);
#     recMove(otherX, y + dy, dy);

#     bigBoard[y][x] = ".";
#     bigBoard[y][otherX] = "."

#     bigBoard[y + dy][x] = isLeft ? "[" : "]";
#     bigBoard[y + dy][otherX] = isLeft ? "]" : "[";
# }

# fn pushBoxinDir(y, x, dx, dy){
#     var x0 = x;
#     var y0 = y;
#     if (dy == 0){
#         var prev = bigBoard[y][x];
#         # move the items one by one until we hit a wall or an '.'
#         while (inBounds(x,y, bigBoard) && (bigBoard[y][x] == "[" || bigBoard[y][x] == "]")){
#             var temp = bigBoard[y][x];
#             bigBoard[y][x] = prev;
#             prev = temp;
#             x += dx;
#         }
#         bigBoard[y0][x0] = ".";
#         bigBoard[y][x] = (dx > 0) ? "]" : "[";
#     }else{
#         recMove(x, y, dy);
#     }
# }

# fn handleP2(i){
#     var move = moves[i];
#     var moveX = movesMap[move][1];
#     var moveY = movesMap[move][0];
#     var newX = rX + moveX;
#     var newY = rY + moveY;
#     if (newX < 0 || newX >= len(bigBoard[0]) || newY < 0 || newY >= len(bigBoard)){
#         return;
#     }
#     if (bigBoard[newY][newX] == "#"){
#         return;
#     }
#     if (bigBoard[newY][newX] == "[" || bigBoard[newY][newX] == "]"){
#         if (canPushBoxinDir(newY, newX, moveX, moveY)){
#             pushBoxinDir(newY, newX, moveX, moveY);
#         }else{
#             return;
#         }
#     }

#     bigBoard[rY][rX] = ".";

#     rX = newX;
#     rY = newY;
#     bigBoard[rY][rX] = "@";
# }

# for (var i = 0; i < len(moves); i += 1){

#     handleP1(i);
#     handleP2(i);

#     if (i % logInterval == 0){
#         print f"i: {i}/{len(moves)}"
#     }
#     # bigBoard.foreach(fn (row){
#     #     print "".join(row);
#     # })
# }


# fn calculateGPSCoors(board, char){
#     var tot = 0;
#     for (var y = 0; y < len(board); y += 1){
#         for (var x = 0; x < len(board[0]); x += 1){
#             if (board[y][x] == char){
#                 tot += (y * 100 + x);
#             }
#         }
#     }

#     return tot;
# }

# # bigBoard.foreach(fn (row){
# #     print "".join(row)
# # })

# print f"GPS: {calculateGPSCoors(board, "O")}"
# print f"GPS: {calculateGPSCoors(board, "@")}"
# print f"GPS: {calculateGPSCoors(bigBoard, "[")}"
# print f"GPS: {calculateGPSCoors(bigBoard, "@")}"



# var input =
# `5,4
# 4,2
# 4,5
# 3,0
# 2,1
# 6,3
# 2,4
# 1,5
# 0,6
# 3,3
# 2,6
# 5,1
# 1,2
# 5,5
# 2,5
# 6,5
# 1,4
# 0,4
# 6,4
# 1,1
# 6,1
# 1,0
# 0,5
# 1,6
# 2,0`


# var boardSize = 7#71
# var numBytes = 12#1024

# input = with(File("./t17"), fn (f) => f.read());
# boardSize = 71
# numBytes = 1024

# var points = input.findall("([0-9]+),([0-9]+)").map(fn (m) => m.map(int))

# # print points
# # print len(points)

# var board = [boardSize].map(fn (_) => [boardSize]{"."})

# points[:numBytes].foreach(fn (p){
#     board[p[1]][p[0]] = "#";
# })

# # board.foreach(fn (row){
# #     print "".join(row);
# # })

# var INF = 1/0

# fn min(a,b){
#     return a < b ? a : b;
# }

# fn dijkstra(x,y, endx,endy){
#     var visited = [boardSize].map(fn (_) => [boardSize]{false})
#     var dist = [boardSize].map(fn (_) => [boardSize]{INF})
#     var q = []
#     q.append({x, y})
#     dist[y][x] = 0;
#     while (len(q) > 0){
#         var p = q.pop(0);
#         var x = p[0];
#         var y = p[1];
#         if (visited[y][x]){
    
#             continue;
#         }
#         visited[y][x] = true;
#         if (x == endx && y == endy || visited[endy][endx]){
#             # return dist[y][x];
#             continue;
#         }
#         if (x > 0 && !visited[y][x - 1] && board[y][x - 1] != "#"){
#             dist[y][x - 1] = min(dist[y][x - 1], dist[y][x] + 1);
#             q.append({x - 1, y});
#         }
#         if (x < boardSize - 1 && !visited[y][x + 1] && board[y][x + 1] != "#"){
#             dist[y][x + 1] = min(dist[y][x + 1], dist[y][x] + 1);
#             q.append({x + 1, y});
#         }
#         if (y > 0 && !visited[y - 1][x] && board[y - 1][x] != "#"){
#             dist[y - 1][x] = min(dist[y - 1][x], dist[y][x] + 1);
#             q.append({x, y - 1});
#         }
#         if (y < boardSize - 1 && !visited[y + 1][x] && board[y + 1][x] != "#"){
#             dist[y + 1][x] = min(dist[y + 1][x], dist[y][x] + 1);
#             q.append({x, y + 1});
#         }
#     }
#     return dist[endy][endx];
# }


# print dijkstra(0,0, boardSize - 1, boardSize - 1)
# for (var i = numBytes+1; i < len(points); i += 1){
#     var p = points[i];
#     board[p[1]][p[0]] = "#";
    
#     if (dijkstra(0,0, boardSize - 1, boardSize - 1) == INF){
#         print f"i: {i} p: {p}"
#         break;
#     }
    
#     if (i % 100 == 0){
#         print f"i: {i}/{len(points)}"
#     }
# }
# # dist.foreach(fn (y, row){
# #    var s = "|"
# #     row.foreach(fn (x, d){
# #         var st;
# #         if (board[y][x] == "#"){
# #             st=" # ";
# #             # return;
# #         }else{
# #             st = d == INF ? "   " : str(d);
# #         }
# #         s = s + ("".join([3 - len(st )]{" "}) + st + "|");
# #     })
# #     print s;
# # })



# var input =
# `px{a<2006:qkq,m>2090:A,rfg}
# pv{a>1716:R,A}
# lnx{m>1548:A,A}
# rfg{s<537:gd,x>2440:R,A}
# qs{s>3448:A,lnx}
# qkq{x<1416:A,crn}
# crn{x>2662:A,R}
# in{s<1351:px,qqz}
# qqz{s>2770:qs,m<1801:hdj,R}
# gd{a>3333:R,R}
# hdj{m>838:A,pv}

# {x=787,m=2655,a=1222,s=2876}
# {x=1679,m=44,a=2067,s=496}
# {x=2036,m=264,a=79,s=2244}
# {x=2461,m=1339,a=466,s=291}
# {x=2127,m=1623,a=2188,s=1013}`

# input = with(File("./t99"), fn (f) => f.read());


# var stuff = input.split("\n\n")

# var workflows =.{}

# var OPS = .{
#     ">" :: fn (a,b) => a > b,
#     "<" :: fn (a,b) => a < b
# }

# class Condition{
#     init(varName, op, val){
#         this.varName = varName;
#         this.op = OPS[op]
#         this.val = val;
#     }
#     eval(map){
#         return this.op(map[this.varName], this.val);
#     }
# }

# class Rule {

#     init(cond, res){
#         this.next = null;
#         this.cond = cond;
#         this.res = res;
#     }

#     leaf(res){
#         return Rule(null, res);
#     }

#     eval(map, workflows){
#         if (this.cond == null){
#             return workflows.get(this.res, this.res);
#         }
#         if (!this.cond.eval(map)){
#             return this.next.eval(map, workflows);
#         }
#         return workflows.get(this.res, this.res);
#     }

# }

# fn parseRule(rule){
#     # rule : "a<2006:qkq"
#     var parts = rule.split(":")
#     if (len(parts) == 1){
#         return Rule.leaf(parts[0])
#     }

#     var cond = parts[0].findall("([a-z])([><])([0-9]+)").map(fn (m) => Condition(m[0], m[1], int(m[2])))[0]
#     var res = parts[1] 
#     return Rule(cond, res);
# }

# fn parseRules(rules){
#     var d = parseRule(rules[0]);
#     var r = d;
#     for (var i = 1; i < len(rules); i += 1){
#         r.next = parseRule(rules[i]);
#         r = r.next;
#     }
#     return d;
# }

# stuff[0].findall("([a-z]+)\{(\S+)\}").map(fn (m) => workflows[m[0]] = parseRules(m[1].split(",")))

# var initial = stuff[1].split("\n").map(fn (s) {
#     var r = .{}
#     s.findall("([a-z])=([0-9]+)").foreach(fn (m) => r[m[0]] = int(m[1]))
#     return r;
# })


# # print initial[0]

# # print workflows["in"].eval(initial[0], workflows)
# # print workflows["gd"].eval(initial[0], workflows)
# fn evalpart(part){
#     var res = workflows["in"];
#     while (true){
#         if (res == "A"){
#             return true;
#         }
#         if (res == "R"){
#             return false;
#         }
#         res = res.eval(part, workflows);
#     }
# }

# var sum = 0
# initial.foreach(fn (i, part){

#     if(evalpart(part)){

#         var tot = 0
#         part.values().foreach(fn (v){
#             tot += v;
#         })
#         sum += tot;

#     # print tot
#     }

# })

# print sum
#  # var d = Rule(null,null);
# # print parseRule("a<2006:qkq")
# # print d /

# # print initial



# var input = 
# `r, wr, b, g, bwu, rb, gb, br

# brwrr
# bggr
# gbbr
# rrbgbr
# ubwu
# bwurrg
# brgr
# bbrgwb`

# input = with(File("./t19"), fn (f) => f.read());

# var stuff = input.split("\n\n")
# var towels = stuff[0].split(", ")
# var designs = stuff[1].split("\n")

# fn max (a,b){
#     return a > b ? a : b;
# }

# var memo = .{}

# fn rec(des, towls){
#     if (len(des) == 0){
#         return 1;
#     }
#     if (memo.containsKey(des)){
#         return memo[des];
#     }
#     # var d = des[0];
#     var res = 0;
    
#     for(var i = 0; i < len(towls); i += 1){
#         var t = towls[i];
#         if (des.match(f"^{t}")){
#             res += rec(des[len(t):], towls);
#         }
#         # print f"t: {t} d: {des}"
#     }
#     memo[des] = res;
#     return res;   
# }

# # print rec("bbrgwb", towels) == len("bbrgwb")
# # print rec(designs[0], towels) == len(designs[0])

# fn checkDesign(des, towels){
#     var s = des;
#     print f"Checking: {des}"
#     towels.foreach(fn (t){ 
#         s = s.trimLeft(t);
#         if (s != des){
#             return t;
#         }
#     })
#     return null;
# }

# print len(designs)
# print len(towels)

# var tot = 0 
# # designs.foreach(fn(i, d){
# for (var i = 0; i < len(designs); i += 1){
#     var d = designs[i];
#     tot += rec(d, towels);
#     # if (rec(d, towels)){
#     #     tot +=1 
#     # }
# }



# print tot
# #     var winners = []
# #     towels.foreach(fn(t){
# #         var s = d.trimLeft(t);
# #         if (s != d){
# #             winners.append(t);
# #             # print f"i: {i} t: {t} d: {d} s: {s}"
# #         }
# #     })
# #     print f"winners for {i}: {winners}"
# # })


# var input = 
# `###############
# #...#...#.....#
# #.#.#.#.#.###.#
# #S#...#.#.#...#
# #######.#.#.###
# #######.#.#...#
# #######.#.###.#
# ###..E#...#...#
# ###.#######.###
# #...###...#...#
# #.#####.#.###.#
# #.#...#.#.#...#
# #.#.#.#.#.#.###
# #...#...#...###
# ###############`

# input = with(File("./t20"), fn (f) => f.read());

# var board = input.split("\n").map(list)
# print f"{len(board)} {len(board[0])}"

# var sIndex = input.find("S")
# var eIndex = input.find("E")

# var sX = sIndex % (len(board[0]) + 1)
# var sY = sIndex // ( len(board) + 1 )

# var eX = eIndex % (len(board[0]) + 1)
# var eY = eIndex // ( len(board) + 1 )


# print f"[{board[sY][sX]}]: {sX},{sY} [{board[eY][eX]}]: {eX},{eY}"

# # var cheats = .{}

# # fn canInjectCheat(x,y, dx,dy){
# #     if(x < 0 || x >= len(board[0]) || y < 0 || y >= len(board)){
# #         return false;
# #     }

# # }

# fn dijkstra(x,y,ex,ey, board){
#     var visited = [len(board)].map(fn (_) => [len(board[0])]{false})
#     var dist = [len(board)].map(fn (_) => [len(board[0])]{1/0})
#     var q = []
#     q.append({x, y})
#     dist[y][x] = 0;
#     while (len(q) > 0){
#         var p = q.pop(0);
#         var x = p[0];
#         var y = p[1];
#         if (visited[y][x]){
#             continue;
#         }
#         visited[y][x] = true;

#         if (x > 0 && !visited[y][x - 1] && board[y][x - 1] != "#"){
#             dist[y][x - 1] = min(dist[y][x - 1], dist[y][x] + 1);
#             q.append({x - 1, y});
#         }
#         if (x < len(board[0]) - 1 && !visited[y][x + 1] && board[y][x + 1] != "#"){
#             dist[y][x + 1] = min(dist[y][x + 1], dist[y][x] + 1);
#             q.append({x + 1, y});
#         }
#         if (y > 0 && !visited[y - 1][x] && board[y - 1][x] != "#"){
#             dist[y - 1][x] = min(dist[y - 1][x], dist[y][x] + 1);
#             q.append({x, y - 1});
#         }
#         if (y < len(board) - 1 && !visited[y + 1][x] && board[y + 1][x] != "#"){
#             dist[y + 1][x] = min(dist[y + 1][x], dist[y][x] + 1);
#             q.append({x, y + 1});
#         }
#     }
#     return dist;    
# }

# fn manhattenDist(x,y,ex,ey){
#     return abs(x - ex) + abs(y - ey);
# }

# var start_to_end = dijkstra(sX, sY, eX, eY, board)
# var end_to_start = dijkstra(eX, eY, sX, sY, board)
# var best_no_cheat = start_to_end[eY][eX]

# print f"start_to_end: {start_to_end[eY][eX]} end_to_start: {end_to_start[sY][sX]}"

# var thres = 100
# var re =[]
# var t = 0
# for (var y = 0; y < len(board); y += 1){
#     for (var x = 0; x < len(board[0]); x += 1){
#         if (board[y][x] == "#" || start_to_end[y][x] > best_no_cheat - thres){
#             continue;
#         }
#         # get all points that are 2 dist away from x y
#         for (var dy = -20; dy <= 20; dy += 1){
#             for (var dx = -20; dx <= 20; dx += 1){
#                 var nx = x + dx;
#                 var ny = y + dy;
#                 # if (nx == x && ny == y){
#                 #     continue;
#                 # }
#                 if (ny < 0 || ny >= len(board) || nx < 0 || nx >= len(board[0])){
#                     continue;
#                 }
#                 var d = manhattenDist(x,y,nx,ny);
#                 if (d <= 20 && (x != nx || y != ny)){ 
#                     if ((start_to_end[y][x]  + end_to_start[ny][nx] + d) <= (best_no_cheat - thres)){
#                         t += 1;
#                         # print f"({x},{y}) -> ({nx},{ny}) = {start_to_end[y][x]  + end_to_start[ny][nx] + 2}"
#                     }
#                 }
#                 else if (d == 20){
#                     print f"({x},{y}) -> ({nx},{ny}) = {d}"
#                 }
#             }
#         }
#     }
# }
# print t
# print len(re)



# var d;
# var input = 
# `029A
# 980A
# 179A
# 456A
# 379A`


# import "stdlib"

# var graph = WeightedDirectedGraph()

# graph.addEdge("A", 3, 1)
# graph.addEdge("A", 0, 1.1)

# graph.addEdge(0, "A", 1)
# graph.addEdge(0, 2, 1)

# graph.addEdge(1, 2, 1)
# graph.addEdge(1, 4, 1)

# graph.addEdge(2, 0, 1)
# graph.addEdge(2, 1, 1.1)
# graph.addEdge(2, 3, 1)
# graph.addEdge(2, 5, 1)

# graph.addEdge(3, "A", 1)
# graph.addEdge(3, 2, 1.1)
# graph.addEdge(3, 6, 1)

# graph.addEdge(4, 1, 1)
# graph.addEdge(4, 5, 1)
# graph.addEdge(4, 7, 1)

# graph.addEdge(5, 2, 1)
# graph.addEdge(5, 4, 1)
# graph.addEdge(5, 6, 1)
# graph.addEdge(5, 8, 1)

# graph.addEdge(6, 3, 1)
# graph.addEdge(6, 5, 1)
# graph.addEdge(6, 9, 1)

# graph.addEdge(7, 4, 1)
# graph.addEdge(7, 8, 1)

# graph.addEdge(8, 5, 1)
# graph.addEdge(8, 7, 1)
# graph.addEdge(8, 9, 1)

# graph.addEdge(9, 6, 1)
# graph.addEdge(9, 8, 1)


# var dirPadGraph = WeightedDirectedGraph()
# dirPadGraph.addEdge("^", "A", 1)
# dirPadGraph.addEdge("^", "v", 1)

# dirPadGraph.addEdge("A", "^", 1)
# dirPadGraph.addEdge("A", ">", 0.9)

# dirPadGraph.addEdge(">", "A", 1)
# dirPadGraph.addEdge(">", "v", 1)

# dirPadGraph.addEdge("<", "v", 0.8)

# dirPadGraph.addEdge("v", ">", 0.9)
# dirPadGraph.addEdge("v", "<", 15)
# dirPadGraph.addEdge("v", "^", 0.9)

# var keypad = {
#     {7,8,9},
#     {4,5,6},
#     {1,2,3},
#     {d,0,"A"}
# }

# var dirpad = {
#     { d , "^", "A"},
#     {"<", "v", ">"},
# }

# var keypadDirMap = .{
#     0 :: .{ "A" :: ">", 2 :: "^"},
#     1 :: .{ 4 :: "^", 2 :: ">"},
#     2 :: .{ 0 :: "v", 1 :: "<", 3 :: ">", 5 :: "^"},
#     3 :: .{ 2 :: "<", 6 :: "^", "A" :: "v"},
#     4 :: .{ 1 :: "v", 5 :: ">"},
#     5 :: .{ 2 :: "v", 4 :: "<", 6 :: ">", 8 :: "^"},
#     6 :: .{ 3 :: "v", 5 :: "<", 9 :: "^"},
#     7 :: .{ 8 :: ">", 4 :: "^"},
#     8 :: .{ 5 :: "v", 7 :: "<", 9 :: ">", 0 :: "^"},
#     9 :: .{ 6 :: "v", 8 :: "<"},
#     "A" :: .{ 0 :: "<", 3 :: "^"}
# }

# var dirDirMap = .{
#     "^" ::  .{
#         "A" :: ">", 
#         "v" :: "v"},
#     "v" ::  .{
#         "<" :: "<", 
#         ">" :: ">", 
#         "^" :: "^"}, 
#     "<" ::  .{
#         "v" :: ">"},
#     ">" ::  .{
#         "v" :: "<", 
#         "A" :: "^"},
#     "A" ::  .{
#         ">" :: "v", 
#         "^" :: "<", 
#         "A" :: "A"}
# }


# var player = {0,2}
# var robot1 = {0,2}
# var robot2 = {3,2}

# print keypadDirMap["A"]
# # print graph

# fn getMoves(start, finish){
#     var path = graph.shortestPath(start, finish)
#     var moves = []
#     for (var i = 0; i < len(path) - 1 ; i += 1){
#         var p = path[i]
#         var n = path[i + 1]
#         moves.append(keypadDirMap[p][n])
#     }
#     moves.append("A")
#     var ret = Object()
#     ret.path = path
#     ret.moves = moves
#     return ret
# }

# var moves = getMoves("A", 0)

# # print f"path: {path}"
# print "".join(moves.moves)

# fn dirPadRun(start, moves){
#     var tp = []
#     var finalMoves = []
#     var p = start
#     for (var i = 0; i < len(moves); i += 1){
#         var pat = dirPadGraph.shortestPath(p, moves[i])
#         # print f"p: '{p}' -> '{moves[i]}' path: {pat}"
#         # pat.append(pat[-1])
#         # movemoves.extend(pat)
#         var gggg = []
#         for (var j = 0; j < len(pat) - 1; j += 1){
#             var p = pat[j]
#             var n = pat[j + 1]
#             var df = dirDirMap[p][n]
#             gggg.append(df)
#             finalMoves.append(df)
#             # print f"p: {p} n: {n} -> {df}"
#         }
#         gggg.append("A")
#         tp.append(gggg)
#         finalMoves.append("A")
#         p = moves[i]
#     }
#     print tp
#     # movemoves.extend(dirPadGraph.shortestPath(p, "A"))

#     # for (var i = 0; i < len(movemoves) - 1; i += 1){
#     #     var p = movemoves[i]
#     #     var n = movemoves[i + 1]
#     #     var df = dirDirMap[p][n]
#     #     print f"p: {p} n: {n} -> {df}"
#     #     finalMoves.append(df)
#     #     finalMoves.append("A")
#     # }
#     # finalMoves.append("A")
#     # print "".join(finalMoves)
#     return finalMoves
# }

# var movesArr = {
#     getMoves("A", 0),
#     getMoves(0, 2),
#     getMoves(2, 9),
#     getMoves(9, "A")
# }

# var a = StringBuilder()
# var b = StringBuilder()
# var start = "A"
# for (var i = 0; i < len(movesArr); i += 1){
#     var moves = movesArr[i]
#     print f"moves: {moves}"
#     var fin = dirPadRun(start, moves.moves)

#     print f"fin: {"".join(fin)}"
#     a.append("".join(fin))
#     var dd = dirPadRun("A", fin)
#     print f"dd: {"".join(dd)}"
#     b.append("".join(dd))
#     # a.append("".join(dirPadRun(fin)))
# }

# print a.toStr()
# print "v<<A>>^A<A>AvA<^AA>A<vAAA>^A\n"
# print b.toStr()
# print "<vA<AA>>^AvAA<^A>A<v<A>>^AvA^A<vA>^A<v<A>^A>AAvA^A<v<A>A>^AAAvA<^A>A"

# print len(a.toStr())
# print len(b.toStr())
# # var fin = dirPadRun(moves)
# # print "TRYING AGAIN!"

# # var x0 = dirPadRun(fin)



# # var movemoves = []
# # var p = "A"
# # for (var i = 0; i < len(moves); i += 1){
# #     var pat = dirPadGraph.shortestPath(p, moves[i])
# #     print f"p: '{p}' -> '{moves[i]}' path: {pat}"
# #     # pat.append(pat[-1])
# #     movemoves.extend(pat)
# #     p = moves[i]
# # }
# # movemoves.append("A")  
# # print f"path {movemoves}"

# # var finalMoves = []
# # for (var i = 0; i < len(movemoves) -1; i += 1){
# #     var p = movemoves[i]
# #     var n = movemoves[i + 1]
# #     var df = dirDirMap[p][n]
# #     print f"p: {p} n: {n} -> {df}"
# #     finalMoves.append(df)
# # }

# # print "".join(finalMoves)

# # print f"final: {dirPadGraph.shortestPath("A", "A")}"
# # print graph.shortestPath("A", 9)
# # print graph.shortestPath(9, 0)


# var input =
# `1
# 10
# 100
# 2024`

# input = 
# `1
# 2
# 3
# 2024`

# # input = with(File("./t22"), fn (f) => f.read());

# var stuff = input.split("\n").map(int)

# fn prune(n){
#     return n % 16777216;
# }

# fn step(n0){
#     n0 = prune(n0 ^ (n0 * 64))
#     n0 = prune(n0 ^ (n0 // 32))
#     return prune(prune(n0 ^ (n0 * 2048)));
# }

# # var g = 123
# # stuff = {g}

# var nth = 10
# fn getNth(n){
#     var r = n % 10
#     var ret = {r}

#     for (var i = 0; i < nth; i += 1){
#         n = step(n)
#         ret.append(n % 10 - r)
#         r = n % 10 
#     }
    
#     return ret 
# }

# var tot = 0

# stuff.map(getNth).foreach(fn (n){
#     print n
# })

# print len(stuff)

# print tot
# # for (var i = 0; i < 10; i += 1){
# #     g = step(g)
# #     print g
# # }



# import "stdlib"

# var input = 
# `kh-tc
# qp-kh
# de-cg
# ka-co
# yn-aq
# qp-ub
# cg-tb
# vc-aq
# tb-ka
# wh-tc
# yn-cg
# kh-ub
# ta-co
# de-co
# tc-td
# tb-wq
# wh-td
# ta-ka
# td-qp
# aq-cg
# wq-ub
# ub-vc
# de-ta
# wq-aq
# wq-vc
# wh-yn
# ka-de
# kh-ta
# co-tc
# wh-qp
# tb-vc
# td-yn`

# input = with(File("./t23"), fn (f) => f.read());

# var stuff = input.split("\n").map(fn (s) => s.split("-"))

# var graph = UndirectedGraph()

# stuff.foreach(fn (s){
#     graph.addEdge(s[0], s[1])
# })



# # print graph

# fn dfs(node, visited, path, graph, parent) {
#     visited[node] = true;
#     path.append(node)
#     graph.adjList[node].foreach(fn (n){
#         if (!visited.containsKey(n)){
#             dfs(n, visited, path, graph, node)

#         }
#     })
#     return path
# }

# fn largestCycle(graph){
#     var visited = .{}

#     var longestPath = []

#     graph.adjList.keys().foreach(fn (node) {
#         if(!visited[node]){
#             var path = []
#             var newPath = dfs(node, visited, path, graph, node)
#             if (len(newPath) > len(longestPath)) {
#                 longestPath = newPath
#             }
#         }
#     })
#     return longestPath
# }

# var groups = [1]
# var longestGroup = -1/0
# graph.adjList.entries().foreach(fn (e){
    
#     for(var i = 0; i < len(e.value); i += 1){
#         var v = e.value[i]
#         var g = {e.key};
#         for (var j = i+1; j < len(e.value); j += 1){
#             var w = e.value[j]
#             # print f"key: {e.key} v: {v} w: {w}"
#             if (graph.hasEdge(v, w)){
#                 # print f"Foundpedge for {e.key}: {v}<->{w}: g: {g}"
#                 var add = true;
#                 for (var k = 1; k < len(g); k += 1){
#                     var inner = g[k]

#                     if ((v != inner && !graph.hasEdge(inner, v)) || (w != inner && !graph.hasEdge(inner, w))){
#                         # print f"inner: {inner} v: {v} w: {w} g: {g} 1: ({(v != inner && !graph.hasEdge(inner, v))}) 2: ({(w != inner && graph.hasEdge(inner, w))})"
#                         add = false;
#                         break;
#                     }
#                 }
#                 if (!add){
#                     continue;
#                 }
#                 # print f"Found edge for {e.key}: {v}<->{w}"
#                 # var toAdd = {e.key, v, w}.sort()
#                 # if (!groups.contains(toAdd)){
#                 #     groups.append(toAdd)
#                 # }
#                 if (!g.contains(v)){
#                     g.append(v)
#                 }
#                 if (!g.contains(w)){
#                     g.append(w)
#                 }
#             }
#         }
#         if (len(g) > 1 && len(g) > longestGroup){
#             groups[0] = g
#             longestGroup = len(g)
#             # print f"Adding group: {g}"
#             # if (!groups.contains(g.sort())){
#             #     groups.append(g)
#             # }
#         }
#     }
# })

# fn printGroup(g){
#     print g
# }
# # groups.filter(fn (g) {
# #     for (var i = 0; i < len(g); i += 1){
# #         var inner = g[i]
# #             if (inner[0] == "t"){
# #                 return false;
# #             }
# #     }
# #     return true;
# # })
# groups.map(fn (x) => x.sort())  
# print groups[0]
# # groups.sort(fn (a,b) 
# # {
# #     if (len(a) > len(b)){
# #         return -1;
# #     }
# #     if (len(a) < len(b)){
# #         return 1;
# #     }
# #     return 0;
# # })
# # print len(groups)
# # print groups[0]

# # var longestRun = largestCycle(graph)

# # print longestRun


# var input = 
# `x00: 1
# x01: 0
# x02: 1
# x03: 1
# x04: 0
# y00: 1
# y01: 1
# y02: 1
# y03: 1
# y04: 1

# ntg XOR fgs -> mjb
# y02 OR x01 -> tnw
# kwq OR kpj -> z05
# x00 OR x03 -> fst
# tgd XOR rvg -> z01
# vdt OR tnw -> bfw
# bfw AND frj -> z10
# ffh OR nrd -> bqk
# y00 AND y03 -> djm
# y03 OR y00 -> psh
# bqk OR frj -> z08
# tnw OR fst -> frj
# gnj AND tgd -> z11
# bfw XOR mjb -> z00
# x03 OR x00 -> vdt
# gnj AND wpb -> z02
# x04 AND y00 -> kjc
# djm OR pbm -> qhw
# nrd AND vdt -> hwm
# kjc AND fst -> rvg
# y04 OR y02 -> fgs
# y01 AND x02 -> pbm
# ntg OR kjc -> kwq
# psh XOR fgs -> tgd
# qhw XOR tgd -> z09
# pbm OR djm -> kpj
# x03 XOR y03 -> ffh
# x00 XOR y04 -> ntg
# bfw OR bqk -> z06
# nrd XOR fgs -> wpb
# frj XOR qhw -> z04
# bqk OR frj -> z07
# y03 OR x01 -> nrd
# hwm AND bqk -> z03
# tgd XOR rvg -> z12
# tnw OR pbm -> gnj`

# input = with(File("./t24"), fn (f) => f.read());

# var stuff = input.split("\n\n")

# var registers = .{}
# var gates = .{}

# class Leaf{
#     init(val){
#         this.val = val;
#     }

#     resolve(){
#         return this.val;
#     }
# }


# class Gate{
#     init(op, a, b, out){
#         this.op = op;
#         this.a = a;
#         this.b = b;
#         this.out = out;
#     }

#     evaluate(){
#         # print f"evaluating {this.out} a: '{this.a}' b: '{this.b}'"
#         this.a = gates[this.a].resolve()
#         this.b = gates[this.b].resolve()
#         registers[this.out] = this.op(this.a, this.b)
#         return registers[this.out]
#     }

#     resolve(){
#         if (registers.containsKey(this.out)){
#             return registers[this.out]
#         }
#         else{

#             var r = gates[this.out].evaluate()
#             registers[this.out] = r
#             return r;
#         }
#     }
# }

# class AndGate <Gate>{
#     init(a, b, out){
#         super.init(fn (a, b) => a & b, a, b, out)
#     }
# }

# class OrGate <Gate>{
#     init(a, b, out){
#         super.init(fn (a, b) => a | b, a, b, out)
#     }
# }

# class XorGate <Gate>{
#     init(a, b, out){
#         super.init(fn (a, b) => a ^ b, a, b, out)
#     }
# }


# stuff[0].split("\n").foreach(fn (s){
#     var parts = s.split(": ")
#     registers[parts[0]] = int(parts[1])
#     gates[parts[0]] = Leaf(int(parts[1]))

# })

# stuff[1].match("([a-z0-9]+) ([A-Z]+) ([a-z0-9]+) -> ([a-z0-9]+)").foreach(fn (m){
#     var a = m[1]
#     var op = m[2]
#     var b = m[3]
#     var out = m[4]
#     # print f"op: {op} a: {a} b: {b} out: {out}"
#     var gate;
#     if (op == "AND"){
#         gate = AndGate(a,b,out)
#     }
#     else if (op == "OR"){
#         gate = OrGate(a,b,out)
#     }
#     else if (op == "XOR"){
#         gate = XorGate(a,b,out)
#     }
#     gates[out] = gate
# })


# gates.values().foreach(fn (e){
#     e.resolve()
# })

# print "".join(registers.entries()
# .filter(fn (e) => e.key[0] != "z")
# .sort(fn (a,b) {
#  if (a.key < b.key){
#      return 1;
#     }
#     if (a.key > b.key){
#         return -1;
#     }
#     return 0;
#  })
#  .map(fn (e) => e.value)) ~

# g.exec("DROP TABLE IF EXISTS users")
# g.exec("CREATE TABLE users (name TEXT, age INT, email TEXT)")
# var r = g.query("SELECT * FROM users")
# import "lang"

# class Person{
#     init(name, age, email){
#         this.name = name;
#         this.age = age;
#         this.email = email;
#     }

#     # toStr(){
#     #     return f"'{this.name}', '{this.age}', '{this.email}'"
#     # }

# }


# class PersonMapper {

#     getAll(db){
#         var p = db.query("SELECT * FROM users limit 1000")
#         return p.rows.map(fn (r) => Person(r[0], r[1], r[2]))
#     }

# }



# # var people = [100]

# # for (var i =0 ; i < 100; i += 1){
# #     people[i] = Person(f"Person{i}", i, f"EMAIL@{i}_.com")
# # }



# # var cols = "('name', 'age', 'email')"

# # var values = ",".join(people[:].map(fn (p) => f"({p})"))
# # 
# # print values

# # var q = f"INSERT INTO users {cols} VALUES {values};"
# fn printRes(res){
#     res.foreach(fn (r){
#         print f"|{",".join(r)}|"
#     })
#     return res;
# }

# printRes(

# var g = SQL("S");
# var dd = with (SQL(
#     "S"
# ), fn(g) => PersonMapper.getAll(g))
    # print(g.query("pragma table_info(users)"))


# # print ""
# # g.query(q)
# # printRes(r)
 
# # printRes(g.query("SELECT count(distinct(name)) as count FROM users --where age > 900 order  by age desc"))
# var s = PersonMapper.getAll(g)
# # g.close()

# "  File '/home/kareem/.local/share/pypoetry/venv/lib/python3.13/site-packages/virtualenv/discovery/py_info.py', line 26\n    return list(OrderedDict.fromkeys(['', *os.environ.get('PATHEXT', '').lower().split(os.pathsep)]))\n                                          ^\nSyntaxError: invalid syntax\n"

~
if (randInt(0, 1)){
    print "Hello"
} else if (randInt(0, 1)){
    print "World"
}
for(var i = 0; i < 10; i += 1){ print "t" }

0x604000001ed0 allocate 40 for STRING
0x603000001930 allocate 32 for LIST
0x604000001f10 allocate 40 for STRING
0x604000001f50 allocate 40 for STRING



0x607000000020 allocate 80 for FUNCTION
0x604000001f90 allocate 40 for CLOSURE
0x604000001fd0 allocate 40 for INSTANCE
0x604000002010 allocate 40 for STRING
0x604000002050 allocate 40 for STRING
0x604000002090 allocate 40 for STRING

0x607000000020 free type FUNCTION
0x604000002090 free type STRING
0x604000002050 free type STRING
0x604000002010 free type STRING
0x604000001fd0 free type INSTANCE
0x604000001f90 free type CLOSURE



~