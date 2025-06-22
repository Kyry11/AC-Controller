const zlib = require('zlib');

// paste buffer here
const htmlBuffer = [

];

// paste string here
const htmlString = String.raw`

`;

if (htmlBuffer.length) {
  zlib.gunzip(Buffer.from(htmlBuffer), (err, decompressed) => {
    if (err) {
      console.error('An error occurred:', err);
      process.exit(1);
    }
    console.log("");
    console.log(decompressed.toString());
  });
}

if (htmlString.trim().length) {
  zlib.gzip(Buffer.from(htmlString), (err, compressed) => {
    if (err) {
      console.error("An error occurred while compressing:", err);
      process.exit(1);
    }
    const bytes = Array.from(compressed).join(',');
    console.log("");
    console.log("Byte count:", bytes.length);
    console.log(bytes);
  });
}
