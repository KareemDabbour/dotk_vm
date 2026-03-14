import "x11"


var WIDTH = 1100
var HEIGHT = 720


class Camera {

    init(){
        this.x = 0.0
        this.y = 0.0
        this.z = -6.0
        this.rotX = 0.0
        this.rotY = 0.0
        this.fov = 620.0
    }

    moveLeft(amount){
        this.x -= amount
    }

    moveRight(amount){
        this.x += amount
    }

    moveUp(amount){
        this.y -= amount
    }
    moveDown(amount){
        this.y += amount
    }

    moveForward(amount){
        this.z += amount
    }
    moveBackward(amount){
        this.z -= amount
    }

    rotateX(amount){
        this.rotX += amount
    }
    rotateY(amount){
        this.rotY += amount
    }
}

class Model{
    init(id, vert, edges, path){
        this.id = id
        this.vert = vert
        this.edges = edges
        this.path = path
    }

    fromPath(path){
        var obj = x11LoadObj(path)
        if(obj == null){
            print_error("Failed to load OBJ model from path: " + path)
            return null
        }
        return Model(hash(path), obj["verts"], obj["edges"], path)
    }

    __str__(){
        return "Model(id=" + this.id + ", vert=" + len(this.vert || {}) + ", edges=" + len(this.edges || {}) + ", path='" + this.path + "')"
    }
}

class InternedModel {
    init(x,y,z, rotX, rotY, scale, path){
        this.x = x
        this.y = y
        this.z = z
        this.rotX = rotX
        this.rotY = rotY
        this.scale = scale
        this.path = path
    }

    draw(cam){
        x11DrawObjWire(this.path, this.x, this.y, this.z, this.scale, this.rotX, this.rotY, cam.x, cam.y, cam.z, cam.rotX, cam.rotY, cam.fov, 255, 255, 255)
    }

    __str__(){
        return "InternedModel(path='" + this.path + "', x=" + this.x + ", y=" + this.y + ", z=" + this.z + ", rotX=" + this.rotX + ", rotY=" + this.rotY + ", scale=" + this.scale + ")"
    }
}

mod = Model.fromPath("models/cow.obj")

print(mod)

# x11Init(WIDTH, HEIGHT, "DotK X11 OBJ Viewer")

mods = {
    InternedModel(0, 0, 0, 0, 0, 1.5, "models/demo_ship.obj"),
    InternedModel(2, 0, 0, 0, 0, 1.5, "models/demo_ship.obj"),
    InternedModel(-2, 0, 0, 0, 0, 1.5, "models/demo_ship.obj"),
    InternedModel(0, 2, 0, 0, 0, 1.5, "models/demo_ship.obj"),
    InternedModel(0, -2, 0, 0, 0, 1.5, "models/demo_ship.obj"),
}

cam = Camera()

var ok = x11Init(WIDTH, HEIGHT, "DotK OBJ Viewer")
if(!ok){
  print_error("x11Init failed")
}

while(!x11ShouldClose()){
    x11Begin(20, 22, 30)
    


    mods.foreach(fn(m) {
        m.draw(cam)
    })



    if(x11KeyDown(X11_KEY_ESCAPE)) break

    x11End()
}