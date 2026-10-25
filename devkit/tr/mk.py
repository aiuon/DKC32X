# transition capture script: boot -> file select -> map -> JH -> pause/exit to map -> walk -> next level
L=[]
def run(n,b=''): L.append(('run %d %s'%(n,b)).strip())
def dense(n, tag, step=2, b=''):
    for i in range(n//step):
        run(step,b); L.append('shot %s%04d'%(tag,i))
run(700); run(5,'START'); run(400); run(5,'START'); run(300); run(5,'START'); run(300); run(5,'START'); run(400); run(5,'START'); run(290)
run(5,'START'); dense(700,'A')          # overview -> Kongo Jungle map
run(5,'START'); dense(700,'B')          # map -> JH
run(300,'RIGHT')
run(5,'START'); run(30)
for k in range(4): run(4,'DOWN'); run(6)
run(5,'B'); dense(700,'C')              # exit to map
run(60,'RIGHT'); run(5,'START'); dense(800,'D')   # next level
print(';'.join(L))
