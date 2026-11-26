// examples/dashboard.fut, driven as a user would (wasm only: std/ui is the DOM's).
import "../../examples/dashboard.fut"
import "std/ui"
import "std/strings"

fn page() { return ui.htmlOf(ui.find("body")) }
fn click(sel) { ui.trigger(ui.find(sel), "click", nil) }
fn wait() { sleep(0.01) }

var FLEET = "\{\"hours\": [\"00:00\",\"01:00\",\"02:00\",\"03:00\",\"04:00\",\"05:00\",\"06:00\",\"07:00\",\"08:00\",\"09:00\",\"10:00\",\"11:00\",\"12:00\",\"13:00\",\"14:00\",\"15:00\",\"16:00\",\"17:00\",\"18:00\",\"19:00\",\"20:00\",\"21:00\",\"22:00\",\"23:00\"],
 \"regions\": [\{\"name\": \"us-east\", \"rpm\": [100,110,120,130,140,150,160,170,180,190,200,210,220,230,240,250,260,270,280,290,300,310,320,330]},
               \{\"name\": \"eu-west\", \"rpm\": [50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50]}],
 \"services\": [\{\"id\": \"auth\", \"name\": \"Auth\", \"team\": \"Identity\", \"region\": \"us-east\", \"status\": \"ok\", \"rpm\": 900, \"errors\": 0.1, \"p95\": 60,
                 \"latency\": [60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60], \"deploys\": [\{\"at\": \"09:12\", \"by\": \"lin\", \"note\": \"bump runtime\"}]},
                \{\"id\": \"images\", \"name\": \"Images\", \"team\": \"Media\", \"region\": \"eu-west\", \"status\": \"down\", \"rpm\": 100, \"errors\": 100, \"p95\": 0,
                 \"latency\": [90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,90,0,0,0], \"deploys\": []}],
 \"incidents\": [\{\"id\": \"INC-1\", \"title\": \"Images returns 503\", \"service\": \"images\", \"severity\": \"high\", \"opened\": \"22:41\", \"acked\": false}]}"

test "Switchyard: a failed load, then the board, the table, a service, an incident" {
  wait()
  expect page().has("The fleet data didn't load.")
  ui.respond("/examples/data/fleet.json", FLEET)
  click("#refresh")
  wait()
  expect page().has("1 of 2 services need") && page().has("Images") && page().has("eu-west · Down")
  expect page().has("Requests a minute") && page().has("<polyline")
  expect page().has("class=\"count\">1<")
  click("#range-6")
  expect page().has("class=\"range on\" id=\"range-6\"")
  ui.go("/services")
  expect page().has("<table") && page().has("Identity")
  click("#sort-rpm")
  click("#sort-rpm")
  expect page().find("row-auth") < page().find("row-images")
  click("#sort-rpm")
  expect page().find("row-images") < page().find("row-auth")
  ui.trigger(ui.find("#search"), "input", {value: "ident"})
  expect page().has("row-auth") && !page().has("row-images")
  click("#only-down")
  expect page().has("No service matches")
  ui.go("/services/images")
  expect page().has("<h1>Images</h1>") && page().has("Images returns 503")
  click("#ack-INC-1")
  expect page().has("Acknowledged") && !page().has("class=\"count\"")
  ui.go("/services/nope")
  expect page().has("There is no service called nope.")
  ui.go("/incidents")
  click("#report")
  expect page().has("<dialog open")
  ui.trigger(ui.find("#report-form"), "submit", nil)
  expect page().has("Say what's wrong in a few words") && page().has("Pick the service it affects.")
  expect page().has("aria-invalid")
  ui.trigger(ui.find("#r-title"), "input", {value: "Logins fail for some users"})
  ui.trigger(ui.find("#r-service"), "change", {value: "auth"})
  expect !page().has("Say what's wrong") && !page().has("Pick the service")
  ui.trigger(ui.find("#report-form"), "submit", nil)
  expect page().has("INC-419") && page().has("Logins fail for some users") && page().has("Reported INC-419.")
  expect !page().has("<dialog open")
  click("#report")
  expect !page().has("Say what's wrong")  // a fresh form: nothing shown as wrong yet
  click("#level-low")
  expect !page().has("id=\"inc-INC-1\"")  // (its toast may still be up)
  click("#level-all")
  expect page().has("id=\"inc-INC-1\"")
  ui.go("/requests")
  expect page().has("20,000 requests today")
  expect strings.count(page(), "class=\"row-slot\"") < 60
  expect !page().has("top: 170000px")
  ui.trigger(ui.find("#log-rows"), "scroll", {top: 170000, height: 600})
  expect page().has("top: 170000px") && strings.count(page(), "class=\"row-slot\"") < 60
  click("#log-errors")
  expect !page().has("20,000 requests today") && page().has("requests today")
}
