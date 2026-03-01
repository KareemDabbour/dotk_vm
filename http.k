import "http"

class HttpClient {
    init(baseUrl){
        this.baseUrl = baseUrl
    }

    _join_(path){
        if (!path || path == ""){
            return this.baseUrl
        }
        if (path[0] == "/"){
            return this.baseUrl + path
        }
        return this.baseUrl + "/" + path
    }

    get(path){
        return http_get(this._join_(path))
    }

    post(path, body){
        return http_post(this._join_(path), body)
    }

    request(method, path, body){
        if (body == null){
            return http_request(method, this._join_(path))
        }
        return http_request(method, this._join_(path), body)
    }
}

fn httpGet(url){
    return http_get(url)
}

fn httpPost(url, body){
    return http_post(url, body)
}

fn httpRequest(method, url, body){
    if (body == null){
        return http_request(method, url)
    }
    return http_request(method, url, body)
}
