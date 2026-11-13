# ECSE3038 - Tutorial 6 starter: the readings API from Friday's lecture.
# Nothing to write in here today. Today's job is to run it on your VM.

from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException, Header, Response
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from typing import cast, Dict, Any
import os
from dotenv import load_dotenv
from pymongo import MongoClient, ASCENDING
from pymongo.errors import DuplicateKeyError

import paho.mqtt.publish as publish
import secrets

load_dotenv()
client = MongoClient(os.getenv("MONGODB_URI"), tz_aware=True)
db = client["tutorial6"]
devices = db["devices"]
readings = db["readings"]
settings = db["settings"]
MQTT_BROKER = "135.232.203.144"
MQTT_NAME = "qwerty2"

devices.create_index([("mac", ASCENDING)], unique=True)

app = FastAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

class Device(BaseModel):
    mac: str
    room: str | None = None


class Reading(BaseModel):
    mac: str
    temp: float = Field(ge=-40, le=80)

class Alarm(BaseModel):
    max_temp: float   
@app.get("/devices")
def get_devices():
    return list(devices.find({}, {"_id": 0}))

@app.post("/devices", status_code=201)
def create_device(device: Device):
    new_device = device.model_dump() #converts to dictionary
    new_device["key"] = secrets.token_urlsafe(16) #generates key into new field
    try:
        devices.insert_one(new_device)
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail=f"Device with MAC {device.mac} already exists.")
    new_device.pop("_id", None)
    return new_device


@app.post("/readings", status_code=201)
def create_reading(reading: Reading, x_api_key: str | None = Header(default=None, alias="X-API-Key")):
    # 1. Check if the key is missing or doesn't belong to ANY device -> 401
    if not x_api_key:
        raise HTTPException(status_code=401, detail="Missing X-API-Key")
    
    key_owner = devices.find_one({"key": x_api_key})
    if key_owner is None:
        raise HTTPException(status_code=401, detail="Invalid X-API-Key")

    # 2. Check if the target device exists -> 404
    device = devices.find_one({"mac": reading.mac})
    if device is None:
        raise HTTPException(status_code=404, detail="No device with MAC " + reading.mac)

    # 3. Real key, but it belongs to another MAC -> 403
    if key_owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="API key does not belong to this device")

    new_reading = reading.model_dump()
    new_reading["time"] = datetime.now(timezone.utc)
    readings.insert_one(new_reading)
    new_reading.pop("_id", None)
    return new_reading

@app.get("/readings")
def get_readings(mac: str | None = None, since: datetime | None = None, limit: int = 10):
    query = {}
    if mac is not None:
        query["mac"] = mac
    if since is not None:
        query["time"] = {"$gte": since}
    return list(readings.find(query, {"_id": 0}).sort("time", -1).limit(limit))[cite: 3]

@app.put("/alarm/{mac}")
def put_settings(mac: str, new: Alarm, response: Response):
    if devices.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="No device with MAC " + mac)
   
    doc = new.model_dump()
    doc["mac"] = mac

    result = settings.replace_one({"mac": mac}, doc, upsert=True)
    if result.upserted_id is not None:
        response.status_code = 201

    doc.pop("_id", None)

    try:
        publish.single(
            "ecse3038/" + MQTT_NAME + "/alarm/max",
            str(new.max_temp),
            retain=True,
            hostname=MQTT_BROKER
        )              
    except Exception as error:
        print("Could not publish the alarm max temp:", error)
    return doc