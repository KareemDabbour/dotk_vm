from lang import Ansi, with
from stdlib import UndirectedGraph
from mathx import INF


class Direction{
    UP=0
    DOWN=1
    LEFT=2
    RIGHT=3
}

fn getKeyInput(){
    var c = getc();
    switch(c){
        case 27{
            getc();
            switch(getc()){
                case 65 {
                    return {-1,0}; # UP
                }
                case 66 {
                    return {1,0}; # DOWN
                }
                case 67 {
                    return {0,1}; #RIGHT
                }
                case 68 {
                    return {0,-1}; #LEFT
                }
            }
        }

        default{
            return c;
        }
    }
}

class Entity{
    init(x=0, y=0, symbol="?"){
        this.coords = {x,y}
        this.spawnCoords = {x,y}
        this.prevCoords = null
        this.stallTicks = 0
        this.symbol = symbol
    }

    getPos(){
        return this.coords;
    }

    move(direction){
        switch(direction){
            case(Direction.UP){
                this.coords[0] -= 1 ;
            }
            case(Direction.DOWN){
                this.coords[0] += 1;
            }
            case(Direction.LEFT){
                this.coords[1] -= 1;
            }
            case(Direction.RIGHT){
                this.coords[1] += 1;
            }
        }
    }

    moveTo(coords){
        this.prevCoords = {this.coords[0], this.coords[1]}
        this.coords = {coords[0], coords[1]}
    }

    getPrevPos(){
        return this.prevCoords;
    }

    resetToSpawn(){
        this.prevCoords = null
        this.stallTicks = 0
        this.coords = {this.spawnCoords[0], this.spawnCoords[1]}
    }

}

class Player <Entity>{
    init(x=0, y=0, symbol="P"){
        super.init(x, y, symbol);
        this.score = 0;
        this.lives = 3;
        this.pelletsEaten = 0;
    }
}



class Ghost <Entity>{
    init(x=0, y=0, symbol="G"){
        super.init(x, y, symbol);
    }

    moveNextMove(board, playerPos){
        var nextMove = board.getNextMove(this.getPos(), playerPos)
        this.moveTo(nextMove);
    }

}

class Field {
    init(rows=0, cols=0){
        this.rows = rows
        this.cols = cols
        this.board = []
        this.entities = []
        this.pelletsLeft = 0
        this.neighbors = []

    }

    resetEntities(){
        for e in this.entities {
            e.resetToSpawn()
        }
    }
    registerEntity(entity){
        this.entities.append(entity)
    }

    idx(x, y){
        return x * this.cols + y;
    }

    sameCoords(a, b){
        if (a == null || b == null) return false
        return a[0] == b[0] && a[1] == b[1]
    }

    containsIdx(indices, value){
        for (var i = 0; i < len(indices); i += 1){
            if (indices[i] == value) return true
        }
        return false
    }

    coordsFromIdx(i){
        var col = i % this.cols
        var row = (i - col) / this.cols
        return {int(row), int(col)}
    }

    inBounds(x, y){
        return x >= 0 && x < this.rows && y >= 0 && y < this.cols;
    }

    isWalkable(x, y){
        return this.inBounds(x, y) && this.board[this.idx(x, y)] != "#";
    }

    moveEntity(entity, dx, dy){
        var currentPos = entity.getPos()
        var nx = currentPos[0] + dx
        var ny = currentPos[1] + dy
        if (this.isWalkable(nx, ny)){
            entity.moveTo({currentPos[0] + dx, currentPos[1] + dy})
            return true
        }
        return false
    }
    moveEntityA(entity, coords){
        if (this.isWalkable(coords[0], coords[1])){
            entity.moveTo({coords[0],  coords[1]})
            return true
        }
        return false
    }

    get(x,y){
        return this.board[this.idx(x, y)];
    }

    parse(lines){
        var trimmed = []
        for (var i = 0; i < len(lines); i += 1){
            var line = trim(lines[i])
            if (line != ""){
                trimmed.append(line)
            }
        }

        this.rows = len(trimmed)
        if (this.rows == 0){
            this.cols = 0
            this.board = []
            this.pelletsLeft = 0
            this.neighbors = []
            return;
        }
        this.cols = len(trimmed[0])
        this.board = []
        this.pelletsLeft = 0

        trimmed
        .map(list)
        .foreach(fn(l){
            this.board.extend(l)
            for ch in l {
                if (ch == "."){
                    this.pelletsLeft += 1
                }
            }
        })

        this.rebuildNeighbors()
    }

    rebuildNeighbors(){
        this.neighbors = []
        for (var i = 0; i < this.rows * this.cols; i += 1){
            this.neighbors.append([])
        }

        for(var i = 0; i < this.rows; i += 1){
            for(var j = 0; j < this.cols; j += 1){
                if (this.isWalkable(i, j)){
                    var curr = this.idx(i, j)
                    for c in {
                        {i - 1, j},
                        {i + 1, j},
                        {i, j - 1},
                        {i, j + 1}
                    } {
                        if (this.isWalkable(c[0], c[1])){
                            this.neighbors[curr].append(this.idx(c[0], c[1]))
                        }
                    }
                }
            }
        }
    }

    neighborIdxsFrom(coords){
        var x = coords[0]
        var y = coords[1]
        var ret = []
        for c in {
            {x - 1, y},
            {x + 1, y},
            {x, y - 1},
            {x, y + 1}
        } {
            if (this.isWalkable(c[0], c[1])){
                ret.append(this.idx(c[0], c[1]))
            }
        }
        return ret
    }

    bfsDistanceFrom(startCoords){
        var dist = []
        for (var i = 0; i < this.rows * this.cols; i += 1){
            dist.append(-1)
        }

        if (!this.isWalkable(startCoords[0], startCoords[1])){
            return dist
        }

        var start = this.idx(startCoords[0], startCoords[1])
        var q = {start}
        var head = 0
        dist[start] = 0

        while (head < len(q)){
            var curr = q[head]
            head += 1
            for n in this.neighbors[curr] {
                if (dist[n] == -1){
                    dist[n] = dist[curr] + 1
                    q.append(n)
                }
            }
        }

        return dist
    }

    getBestMoveToward(fromCoords, dist, blockedIdx = [], prevCoords = null){
        var currIdx = this.idx(fromCoords[0], fromCoords[1])
        var chosen = null
        var nd = this.neighborIdxsFrom(fromCoords)

        var minD = INF
        var bestNoPrev = []
        for (var i = 0; i < len(nd); i += 1){
            var n = nd[i]
            if (this.containsIdx(blockedIdx, n)) continue;
            var c = this.coordsFromIdx(n)
            if (this.sameCoords(c, prevCoords)) continue;
            var d = dist[n]
            if (d == -1) continue;
            if (d < minD){
                minD = d
                bestNoPrev = {n}
            }else if (d == minD){
                bestNoPrev.append(n)
            }
        }
        if (len(bestNoPrev) > 0){
            chosen = bestNoPrev[randInt(0, len(bestNoPrev) - 1)]
        }

        if (chosen == null){
            minD = INF
            var bestWithPrev = []
            for (var i = 0; i < len(nd); i += 1){
                var n = nd[i]
                if (this.containsIdx(blockedIdx, n)) continue;
                var d = dist[n]
                if (d == -1) continue;
                if (d < minD){
                    minD = d
                    bestWithPrev = {n}
                }else if (d == minD){
                    bestWithPrev.append(n)
                }
            }
            if (len(bestWithPrev) > 0){
                chosen = bestWithPrev[randInt(0, len(bestWithPrev) - 1)]
            }
        }

        if (chosen == null){
            var anyNoPrev = []
            for (var i = 0; i < len(nd); i += 1){
                var n = nd[i]
                if (this.containsIdx(blockedIdx, n)) continue;
                var c = this.coordsFromIdx(n)
                if (!this.sameCoords(c, prevCoords)){
                    anyNoPrev.append(n)
                }
            }
            if (len(anyNoPrev) > 0){
                chosen = anyNoPrev[randInt(0, len(anyNoPrev) - 1)]
            }
        }

        if (chosen == null){
            var any = []
            for (var i = 0; i < len(nd); i += 1){
                var n = nd[i]
                if (!this.containsIdx(blockedIdx, n)){
                    any.append(n)
                }
            }
            if (len(any) > 0){
                chosen = any[randInt(0, len(any) - 1)]
            }
        }

        var out = chosen == null ? fromCoords : this.coordsFromIdx(chosen)
        if (!this.isWalkable(out[0], out[1])){
            return fromCoords
        }
        return out
    }

    randomNeighbor(fromCoords, prevCoords = null){
        var nd = this.neighborIdxsFrom(fromCoords)
        if (len(nd) == 0){
            return fromCoords
        }

        var options = []
        for n in nd {
            var c = this.coordsFromIdx(n)
            if (!this.sameCoords(c, prevCoords)){
                options.append(n)
            }
        }

        if (len(options) == 0){
            options = nd
        }

        return this.coordsFromIdx(options[randInt(0, len(options) - 1)])
    }

    chooseGhostStep(fromCoords, dist, prevCoords = null){
        var nd = this.neighborIdxsFrom(fromCoords)
        if (len(nd) == 0){
            return fromCoords
        }

        var minD = INF
        var best = []
        for (var i = 0; i < len(nd); i += 1){
            var n = nd[i]
            var c = this.coordsFromIdx(n)
            if (len(nd) > 1 && this.sameCoords(c, prevCoords)) continue;

            var d = dist[n]
            if (d != -1 && d < minD){
                minD = d
                best = {n}
            }else if (d != -1 && d == minD){
                best.append(n)
            }
        }

        if (len(best) > 0){
            return this.coordsFromIdx(best[randInt(0, len(best) - 1)])
        }

        return this.randomNeighbor(fromCoords, prevCoords)
    }

    consumeAt(pos){
        var i = this.idx(pos[0], pos[1])
        if (this.board[i] == "."){
            this.board[i] = " "
            this.pelletsLeft -= 1
            return true
        }
        return false
    }

    getBoardGraph(){
        var ret = UndirectedGraph()
        for(var i = 0; i < this.rows; i+=1){
            for(var j = 0; j < this.cols; j+=1){
                if (this.board[i * this.cols + j] != "#"){
                    # Add edges to neighboring cells
                    for n in {
                        {i-1, j},
                        {i+1, j},
                        {i, j-1},
                        {i, j+1}
                    } {
                        # print(f"Processing neighbor: {n}, {this.get(n[0], n[1])}")
                        if (n[0] >= 0 && n[0] < this.rows && n[1] >= 0 && n[1] < this.cols && this.board[n[0] * this.cols + n[1]] != "#"){
                            ret.addEdge({i, j}, {n[0], n[1]});
                        }
                    }
                }

            }
        }

        return ret
    }
    getShortestPath(frm, to){
        var g = this.getBoardGraph();
        return g.shortestPath(frm, to);
    }
    getNextMove(frm, to){
        var path = this.getShortestPath(frm, to);
        if (len(path) > 1){
            return path[1];
        }
        return frm;
    }

    getEntityOverlays(){
        var ret = list(this.board)

        for e in this.entities {
            var pos = e.getPos()
            ret[pos[0] * this.cols + pos[1]] = e.symbol
        }
        return ret;
    }

    overlayPath(path){
        var retL = this.getEntityOverlays()
        for p in path {
            retL[p[0] * this.cols + p[1]] = f"{Ansi.GREEN}{retL[p[0] * this.cols + p[1]]}{Ansi.RESET}"
        }
      
        var ret = []
        for(var i = 0; i < this.rows; i+=1){
            ret.append("".join(retL[i * this.cols:(i + 1) * this.cols]));
        }
        return "\n".join(ret);

    }

    toStr(){
        var ret = []
        var overlay = this.getEntityOverlays()
        for(var i = 0; i < this.rows; i+=1){
            ret.append("".join(overlay[i * this.cols:(i + 1) * this.cols]));
        }
        return "\n".join(ret);
    }

}

fn printScoreAndField(player, field){
    print f"LIVES: {player.lives}   SCORE: {player.score}   PELLETS LEFT: {field.pelletsLeft}";
    print(field.toStr());
}

fn frameString(player, field, ghostIntervalNow){
   
    # lines.append(f"LIVES: {player.lives}   SCORE: {player.score}   PELLETS LEFT: {field.pelletsLeft}")
    # lines.append(field.toStr())
    # lines.append(f"<p> pause -- <q> quit -- GHOST STEP: {round(ghostIntervalNow, 3)}s")

    return  "\n".join({
    "\e[H\e[J",
    f"LIVES: {player.lives}   SCORE: {player.score}   PELLETS LEFT: {field.pelletsLeft}",
    field.toStr(),f"<p> pause -- <q> quit -- GHOST STEP: {round(ghostIntervalNow, 3)}s"})
}

fn pauseFrameString(player, field){
    
    return "\n".join({"\e[H\e[J",
    f"LIVES: {player.lives}   SCORE: {player.score}   PELLETS LEFT: {field.pelletsLeft}",
    field.toStr(),
    "<p> to un-pause -- <q> to quit"})
}

fn collided(player, ghosts){
    var ppos = player.getPos()
    for (var i = 0; i < len(ghosts); i += 1){
        var gpos = ghosts[i].getPos()
        if (ppos[0] == gpos[0] && ppos[1] == gpos[1]){
            return true
        }
    }
    return false
}

# fn resetRound(player, ghosts){
#     player.resetToSpawn()
#     ghosts.foreach(fn(g){
#         g.resetToSpawn()
#     })
# }

fn ghostStepInterval(player, totalPelletsAtStart){
    if (totalPelletsAtStart <= 0){
        return 0.06
    }

    var progress = player.pelletsEaten / totalPelletsAtStart
    if (progress < 0){
        progress = 0
    }
    if (progress > 1){
        progress = 1
    }

    # Ramp from slower ghosts at start to faster ghosts near the end.
    var startInterval = 0.20
    var endInterval = 0.06
    return startInterval - ((startInterval - endInterval) * progress)
}

fn advanceGhost(gh, dist){
    var curr = gh.getPos()
    var next = fi.chooseGhostStep(curr, dist, gh.getPrevPos())

    var moved = false
    if (!fi.sameCoords(next, curr)){
        moved = fi.moveEntityA(gh, next)
    }

    if (!moved){
        gh.stallTicks += 1
        if (gh.stallTicks >= 1){
            var fallback = fi.randomNeighbor(curr, gh.getPrevPos())
            moved = fi.moveEntityA(gh, fallback)
            if (moved){
                gh.stallTicks = 0
            }
        }
    }else{
        gh.stallTicks = 0
    }
}



var fi = Field()

var p = Player(1,1, f"{Ansi.GREEN}C{Ansi.RESET}")
var g = Ghost(29,1, f"{Ansi.RED}G{Ansi.RESET}")
var g2 = Ghost(29,25, f"{Ansi.CYAN}H{Ansi.RESET}")


with(File("test.txt"), fn(f){
    var lines = f.readLines()
    fi.parse(lines)
})
        
fi.registerEntity(p)
fi.registerEntity(g)
fi.registerEntity(g2)
var ghosts = {g, g2}
fi.consumeAt(p.getPos())
var totalPelletsAtStart = fi.pelletsLeft > 0 ? fi.pelletsLeft : 1
var dx = 0
var dy = 1
var desiredDx = 0
var desiredDy = 1

fn run(){
    var d
    var c
    var playerStepInterval = 0.095
    var playerTimer = 0
    var ghostTimer = 0
    var targetFrame = 1.0 / 75.0

    # Clear once and hide cursor for smoother terminal animation.
    clear()
    print("\e[?25l")

    while(true){
        var delta = targetFrame

        var ghostIntervalNow = ghostStepInterval(p, totalPelletsAtStart)
        print(frameString(p, fi, ghostIntervalNow))

        if(kbhit()){
            var latestTurn = null
            while(kbhit()){
                d = getKeyInput();
                if(d == 112){
                    print(pauseFrameString(p, fi))
                    while(true){
                        if(kbhit()){
                            c = getKeyInput();
                            if(c == 112){
                                break;
                            }else if(c == 113){
                                return "QUIT"
                            }
                        }
                        sleep(0.016)
                    }
                }else if(d == 113){
                    return "QUIT"
                }else if(len(d) == 2){
                    latestTurn = d
                }
            }

            if (latestTurn != null){
                # Apply only the freshest direction input to reduce turn lag.
                desiredDx = latestTurn[0]
                desiredDy = latestTurn[1]
            }
        }

        playerTimer += delta
        ghostTimer += delta

        if (playerTimer >= playerStepInterval){
            playerTimer -= playerStepInterval

            var pos = p.getPos()
            if(fi.isWalkable(pos[0] + desiredDx, pos[1] + desiredDy)){
                dx = desiredDx
                dy = desiredDy
            }

            fi.moveEntity(p, dx, dy)
            if (fi.consumeAt(p.getPos())){
                p.score += 10
                p.pelletsEaten += 1
            }

            if (fi.pelletsLeft <= 0){
                return "WIN"
            }

            if (collided(p, ghosts)){
                p.lives -= 1
                if (p.lives <= 0){
                    return "DIED"
                }
                fi.resetEntities()
                playerTimer = 0
                ghostTimer = 0
            }
        }

        var ghostInterval = ghostStepInterval(p, totalPelletsAtStart)
        if (ghostTimer >= ghostInterval){
            ghostTimer -= ghostInterval

            var dist = fi.bfsDistanceFrom(p.getPos())
            advanceGhost(g, dist)

            # Keep H on a guaranteed legal random walk so it never freezes.
            var hCurr = g2.getPos()
            var hNext = fi.randomNeighbor(hCurr, g2.getPrevPos())
            var hMoved = fi.moveEntityA(g2, hNext)
            if (!hMoved){
                # Last-resort: ignore previous-position avoidance.
                hNext = fi.randomNeighbor(hCurr, null)
                fi.moveEntityA(g2, hNext)
            }

            if (collided(p, ghosts)){
                p.lives -= 1
                if (p.lives <= 0){
                    return "DIED"
                }
                fi.resetEntities()
                playerTimer = 0
                ghostTimer = 0
            }
        }

        sleep(targetFrame)
    }
}

var outcome = run()
clear()
print("\e[?25h")
printScoreAndField(p, fi)

if (outcome == "WIN"){
    print("YOU WIN!")
}else if (outcome == "DIED"){
    print("YOU DIED")
}else{
    print("GOODBYE!")
}