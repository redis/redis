function validate_schema(command_schema) {
    var error_status = false
    const Ajv = require("ajv/dist/2019")
    const ajv = new Ajv({strict: true, strictTuples: false})
    let json = require('../src/commands/'+ command_schema);
    for (var item in json) {
        const schema = json[item].reply_schema
        if (schema ==  undefined)
            continue;
        try {
            ajv.compile(schema)
        } catch (error) {
            console.error(command_schema + " : " + error.toString())
            error_status = true
        }
    }
    return error_status
}

const schema_directory_path = './src/commands'
const path = require('path')
var fs = require('fs');
var files = fs.readdirSync(schema_directory_path);
jsonFiles = files.filter(el => path.extname(el) === '.json')
var error_status = false
jsonFiles.forEach(function(file){
    if (validate_schema(file))
        error_status = true
})
if (error_status)
    process.exit(1)
