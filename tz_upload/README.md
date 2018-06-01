# tz_upload
Utility to upload binary installer .tgz to the Tzumi Magic TV

This is a result of reverse-engineering the "Updateserver" binary on the device.  It doesn't currently make use of "UpdateServerV2"

# Usage

tz_upload filename.tgz [optional filename on device (experimental)]

# Example installer .tgz files

See example_installers for example script to perform various operations on the device


# Tzumi's update servers

The Tzumi Magic TV runs two separate update servers: **/app/Updateserver** and **/app/UpdateServerV2**

It appears they have an "old" update API (**/app/Updateserver**) and a new one (**/app/UpdateServer2**).

I decided to reverse-engineer and implement a client for the "old" API, since it seemed easiest to do.

### Server startup

Both **/app/Updateserver** and **/app/UpdateServerV2** are started by the **/app/update_daemon.sh** script.

**/app/Updateserver** starts up and listens for client connections on port **6667**.

**/app/UpdateServerV2** starts up and listens for client connections on port **6668**.


### /app/Updateserver (version 1) upload/install flow

Once the server starts up, it's listening for client connections on port 6667.

As soon as a client successfully does a **connect()** to the server, the server will respond with a "ready ack" response of **"SerOK"**

The client then tells the server about the file it wants to upload (filename, filesize, and sha1 hash).  This is in a single **"INFO"** message, with each field delimited by the **"<"** character.

Importantly, the filename specified in the INFO message is the name of the file as it will be stored (in "/tmp") on the device, not necessarily the name of the file on the client PC.

For example, if we are uploading a .tgz file that's 218 bytes long, with a sha1 hash of "c3f38066653248951fb45182f31140581ece9f6b", the INFO string would look like this (I've chosen an arbitrary filename of "upload.tgz"):

`"INFO<upload.tgz<218<c3f38066653248951fb45182f31140581ece9f6b"`

After the client sends the INFO message to the server, the server responds with **"READY"**.

Next, the client uploads the .tgz file.  When the server receives the .tgz, it will calculate a sha1 hash of the receive file, and make sure it matches the value that came in the INFO message.

If the sha1 hash matches what was expected, then the server responds with **"CHECK"**.  Otherwise, it responds with **"UCHEC"** indicating an error.

On a successful upload, the server will then run **"tar xzvf"** on the received image (in "/tmp").  The .tgz file should have a single top-level directory with an "install.sh" script inside, for example "installer/install.sh".

After the file is un-tarred, the server will run the **"/tmp/{directoryname}/install.sh"** script.  If the script wants to return a result back to the client, it should write it to a file called **"/tmp/run_result"**.  I believe the size of this result is limited - maybe to 4 bytes (so, maybe a numeric code and nothing more can be written here)

If the "install.sh" script created "/tmp/run_result", then the contents of that file will be sent back to the client.  Otherwise, a "0" string is sent back.


### upload image filenames

As mentioned above, the INFO message specifies the name of the file as it will be stored on the device.  The server prepends "/tmp/" to whatever the client give it.  So, if the client includes "upload.tgz" in the INFO message, the server will write that file to "/tmp/upload.tgz".

But, the server is stupid.  You can form any path on the device by starting your INFO filename with "../" (for example: "../etc/dummy.dat".  The server will prepend "/tmp/", resulting in a full pathname of "/tmp/../etc/dummy.dat".  The result is that the file you upload won't be in "/tmp" - it will be in "/etc" (or wherever).

If you use this 'trick', it will let you write files to anywhere on the device filesystem.  But, because the server expects the installer to be in "/tmp", the embedded "install.sh" script may not actually run.

I **have NOT** tried this, but I think you could use **tz_loader** to upload arbitrary (NON-tgz) files to anywhere in the filesystem.  It would upload OK, but the install would fail.

A simple example (not that this is helpful) would be to create your own "hosts" file, then upload it to the device with:

`tz_loader ./hosts /etc/hosts`

**tz_loader** recognizes the fact that you have specified a "target" filename (second argument), and will automatically prepend "../" to that in the INFO message.  The "./hosts" file will be uploaded to "/tmp/../etc/hosts", which is "/etc/hosts".  The "untar" in "/tmp" would fail, so the embedded "install.sh" would never be run.

As I said, I have NOT tried the above.  You should also be careful about the resulting file permissions (*it's probably NOT a good idea to try to upload **"/etc/passwd"** and/or **"/etc/shadow"** files, for example!*).


## example upload console output

Here's the console output from **/app/Updateserver** while uploading a simple installer .tgz file (one that just outputs "This is install.sh" to the console)

1970-01-01-00-02-159: listening...

1970-01-01-00-02-162: Client 192.168.1.214 connect success

1970-01-01-00-02-162: Send the ready ack

1970-01-01-00-02-162: Read info ack

1970-01-01-00-02-162: Get the upload info:

1970-01-01-00-02-162: file name:upload.tgz

1970-01-01-00-02-162: file size:218

1970-01-01-00-02-162: file sha1:c3f38066653248951fb45182f31140581ece9f6b

1970-01-01-00-02-162: read 218 bytes --- write 218 bytes

1970-01-01-00-02-162: get file:upload.tgz

success

1970-01-01-00-02-162: save the file:/tmp/upload.tgz

1970-01-01-00-02-162: file size:218

1970-01-01-00-02-162: test size:218

1970-01-01-00-02-162: file sha1:c3f38066653248951fb45182f31140581ece9f6b

1970-01-01-00-02-162: test size:c3f38066653248951fb45182f31140581ece9f6b

1970-01-01-00-02-162: check file success

1970-01-01-00-02-162: getDirNameStart :cd /tmp && rm -f old.p new.p && touch old.p && touch new.p && ls > old.p

installer/

installer/install.sh

1970-01-01-00-02-162: getDirName run1:cd /tmp && ls > new.p

1970-01-01-00-02-162: getDirNamerun2:cd /tmp && grep -vFf old.p new.p

1970-01-01-00-02-162: getDirNameEnd:cd /tmp && rm -f old.p new.p

1970-01-01-00-02-162: getDirName result: installer

1970-01-01-00-02-162: run: cd /tmp/installer && chmod +x install.sh && ./install.sh && rm -rf /tmp/installer /tmp/upload.tgz

This is install.sh

1970-01-01-00-02-163: run result:123

1970-01-01-00-02-163: close the connect

1970-01-01-00-02-163: listening...
