import json

with open('data.json') as f:
    data = json.load(f)

contents = ""

for cell in data:
    if(not cell['special']):
       contents += cell['content']
       contents += "\n"

with open('contents_combined.txt', 'w') as f:
    f.write(contents)
